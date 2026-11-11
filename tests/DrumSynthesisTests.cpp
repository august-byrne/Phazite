// SPDX-License-Identifier: AGPL-3.0-or-later
// The drums Phazite synthesizes (src/devices/drums/DrumSynthesis.h), where a shape's parts are measured: a kick's attack,
// body boost and click, a hat held and cut, a clap's strikes and width, what is struck, a room that isn't gated, a snare's
// further modes and a Simmons drum's hold, each doing what it says and nothing at its default; their noise, the same on
// every build; and the Dark Synthwave Kit's sixteen sounds (FactoryDrums.h), made of them.
#include "TestHelpers.h"
#include "devices/drums/DrumSampler.h"
#include "devices/drums/DrumSynthesis.h"
#include "devices/drums/FactoryDrums.h"

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
namespace devices = phazite::devices;
namespace drums = phazite::devices::drums;
using drums::Sound;
constexpr double rate = 48000.0;

int at (double seconds)
{
    return (int) std::lround (seconds * rate);
}

// The level of a stretch of one side of a sound, as its root mean square.
double level (const Sound& sound, double from, double to, int channel = 0)
{
    const auto start = std::min (at (from), sound.getNumSamples()), end = std::min (at (to), sound.getNumSamples());
    auto sum = 0.0;

    for (int i = start; i < end; ++i)
        sum += (double) sound.getSample (channel, i) * sound.getSample (channel, i);

    return end > start ? std::sqrt (sum / (end - start)) : 0.0;
}

double decibels (double gain)
{
    return 20.0 * std::log10 (std::max (gain, 1.0e-12));
}

// How much of one frequency a stretch of a sound holds.
double toneLevel (const Sound& sound, double hz, double from, double to)
{
    const auto start = at (from), end = std::min (at (to), sound.getNumSamples());
    auto real = 0.0, imaginary = 0.0;

    for (int i = start; i < end; ++i)
    {
        const auto window = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * (i - start) / (end - start));
        const auto phase = juce::MathConstants<double>::twoPi * hz * i / rate;
        real += window * sound.getSample (0, i) * std::cos (phase);
        imaginary += window * sound.getSample (0, i) * std::sin (phase);
    }

    return std::hypot (real, imaginary) / (end - start);
}

// A sound's band between two frequencies (24 dB an octave at each end).
Sound bandOf (const Sound& sound, double lowHz, double highHz)
{
    auto band = sound;

    for (int channel = 0; channel < band.getNumChannels(); ++channel)
    {
        for (int pass = 0; pass < 2; ++pass)
        {
            juce::IIRFilter low, high;
            low.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, lowHz));
            high.setCoefficients (juce::IIRCoefficients::makeLowPass (rate, highHz));
            low.processSamples (band.getWritePointer (channel), band.getNumSamples());
            high.processSamples (band.getWritePointer (channel), band.getNumSamples());
        }
    }

    return band;
}

// How alike a stretch of a stereo sound's two sides are: 1 the same, 0 unrelated.
double correlation (const Sound& sound, double from, double to)
{
    auto both = 0.0, left = 0.0, right = 0.0;

    for (int i = at (from); i < std::min (at (to), sound.getNumSamples()); ++i)
    {
        both += (double) sound.getSample (0, i) * sound.getSample (1, i);
        left += (double) sound.getSample (0, i) * sound.getSample (0, i);
        right += (double) sound.getSample (1, i) * sound.getSample (1, i);
    }

    return left > 0.0 && right > 0.0 ? both / std::sqrt (left * right) : 0.0;
}

// A stereo sound's sides' difference against what they share, in decibels.
double sideAgainstMid (const Sound& sound, double from, double to)
{
    auto side = 0.0, mid = 0.0;

    for (int i = at (from); i < std::min (at (to), sound.getNumSamples()); ++i)
    {
        side += std::pow ((double) sound.getSample (0, i) - sound.getSample (1, i), 2.0);
        mid += std::pow ((double) sound.getSample (0, i) + sound.getSample (1, i), 2.0);
    }

    return 10.0 * std::log10 (std::max (side, 1.0e-20) / std::max (mid, 1.0e-20));
}

// Both sides' level together.
double levelOfBoth (const Sound& sound, double from, double to)
{
    return std::sqrt (0.5 * (std::pow (level (sound, from, to, 0), 2.0) + std::pow (level (sound, from, to, 1), 2.0)));
}

// One side of a sound, from its start.
Sound partOf (const Sound& sound, int numSamples, int channel = 0)
{
    Sound part (1, numSamples);
    part.copyFrom (0, 0, sound, channel, 0, numSamples);
    return part;
}

bool areSame (const Sound& a, const Sound& b)
{
    if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
        return false;

    for (int channel = 0; channel < a.getNumChannels(); ++channel)
        if (std::memcmp (a.getReadPointer (channel), b.getReadPointer (channel), sizeof (float) * (size_t) a.getNumSamples()) != 0)
            return false;

    return true;
}

// How often a sound's low band crosses zero in a stretch: its pitch, roughly.
int countCrossings (const Sound& sound, double from, double to)
{
    const auto low = bandOf (sound, 40.0, 500.0);
    auto crossings = 0;

    for (int i = at (from) + 1; i < std::min (at (to), low.getNumSamples()); ++i)
        crossings += (low.getSample (0, i - 1) < 0.0f) != (low.getSample (0, i) < 0.0f) ? 1 : 0;

    return crossings;
}

std::shared_ptr<const devices::DrumSampler::Sound> getDrum (const char* name)
{
    const auto index = devices::findFactoryDrum (name);
    REQUIRE (index >= 0);
    return devices::getFactoryDrum (index);
}
} // namespace

TEST_CASE ("The drums' own noise is seeded: the same from a seed and a stream on every build, another from any other")
{
    auto first = drums::makeRandom (1), again = drums::makeRandom (1), other = drums::makeRandom (2), stream = drums::makeRandom (1, 1);
    auto sum = 0.0, squares = 0.0;
    auto sameAsOther = 0, sameAsStream = 0;

    for (int i = 0; i < 20000; ++i)
    {
        const auto value = drums::noise (first);
        CHECK (value == drums::noise (again));
        sameAsOther += juce::exactlyEqual (value, drums::noise (other)) ? 1 : 0;
        sameAsStream += juce::exactlyEqual (value, drums::noise (stream)) ? 1 : 0;
        REQUIRE (value >= -1.0f);
        REQUIRE (value < 1.0f);
        sum += value;
        squares += (double) value * value;
    }

    CHECK (sameAsOther < 5);
    CHECK (sameAsStream < 5);
    CHECK (std::abs (sum / 20000.0) < 0.02);                                          // around nothing
    CHECK (std::sqrt (squares / 20000.0) == doctest::Approx (0.5774).epsilon (0.02)); // evenly from -1 to 1

    // Whole numbers only, so every compiler draws these: the generators' first draws for two seeds and streams.
    CHECK (drums::makeRandom (1).nextInt() == 0x944e0752u);
    CHECK (drums::makeRandom (7, 2).nextInt() == 0xcfea46fcu);
}

TEST_CASE ("A kick's attack, body boost, shelf and click shape it as they say, and leave it alone at their defaults")
{
    const drums::KickShape
        plain { .startHz = 120.0, .endHz = 55.0, .pitchSeconds = 0.03, .decaySeconds = 0.15, .click = 0.0, .length = 0.4, .drive = 1.0 };
    const auto kick = drums::makeKick (rate, plain);

    // No boost and no shelf at 0 dB, wherever they are set; the click as wide as it was.
    auto same = plain;
    same.boostHz = 200.0;
    same.boostQ = 3.0;
    same.shelfHz = 30.0;
    same.clickQ = 0.8;
    same.attackSeconds = 0.0005;
    CHECK (areSame (kick, drums::makeKick (rate, same)));

    SUBCASE ("its attack lets the body swell in")
    {
        auto slow = plain;
        slow.attackSeconds = 0.02;
        const auto swelling = drums::makeKick (rate, slow);
        CHECK (level (swelling, 0.0, 0.005) < 0.4 * level (kick, 0.0, 0.005));
        CHECK (level (swelling, 0.03, 0.06) == doctest::Approx (level (kick, 0.03, 0.06)).epsilon (0.02)); // and no more
    }

    SUBCASE ("its boost makes the body boom where it is set, at the same peak")
    {
        auto boosted = plain;
        boosted.boostHz = 55.0;
        boosted.boostQ = 2.0;
        boosted.boostDecibels = 12.0;
        const auto booming = drums::makeKick (rate, boosted);
        const auto peak = booming.getMagnitude (0, booming.getNumSamples()), before = kick.getMagnitude (0, kick.getNumSamples());
        CHECK (peak <= 1.0001f);
        CHECK (std::abs (decibels (peak) - decibels (before)) < 1.0);

        // Its tail, down at 55 Hz, against its start, up at 120 Hz: more of it.
        const auto tailBefore = decibels (level (kick, 0.15, 0.3) / level (kick, 0.0, 0.01));
        const auto tailAfter = decibels (level (booming, 0.15, 0.3) / level (booming, 0.0, 0.01));
        CHECK (tailAfter > tailBefore + 6.0);

        // A cut does the opposite.
        boosted.boostDecibels = -12.0;
        const auto thin = drums::makeKick (rate, boosted);
        CHECK (decibels (level (thin, 0.15, 0.3) / level (thin, 0.0, 0.01)) < tailBefore - 6.0);
    }

    SUBCASE ("its shelf lifts what is under it")
    {
        // A kick that starts far above the shelf and lands under it: its tail against its start.
        drums::KickShape falling { .startHz = 300.0,
                                   .endHz = 50.0,
                                   .pitchSeconds = 0.02,
                                   .decaySeconds = 0.15,
                                   .click = 0.0,
                                   .length = 0.4,
                                   .drive = 1.0 };
        const auto unshelved = drums::makeKick (rate, falling);
        falling.shelfHz = 90.0;
        falling.shelfDecibels = 12.0;
        const auto low = drums::makeKick (rate, falling);
        const auto tailBefore = decibels (level (unshelved, 0.15, 0.3) / level (unshelved, 0.0, 0.004));
        CHECK (decibels (level (low, 0.15, 0.3) / level (low, 0.0, 0.004)) > tailBefore + 6.0);
    }

    SUBCASE ("its click is as wide as its Q says")
    {
        // Noise around 2 kHz over a body far below it: how much of the click is near its centre, against far above it.
        drums::KickShape
            clicking { .startHz = 40.0, .endHz = 40.0, .click = 3.0, .clickHz = 2000.0, .clickSeconds = 0.01, .length = 0.1, .drive = 0.5 };
        const auto spread = [&] (double q)
        {
            clicking.clickQ = q;
            const auto made = drums::makeKick (rate, clicking);
            return decibels (level (bandOf (made, 1700.0, 2300.0), 0.0, 0.03) / level (bandOf (made, 6000.0, 12000.0), 0.0, 0.03));
        };

        CHECK (spread (4.0) > spread (0.5) + 6.0);
    }

    SUBCASE ("a knock is the same whatever the seed, and noise isn't")
    {
        drums::KickShape knocking { .startHz = 40.0,
                                    .endHz = 40.0,
                                    .click = 2.0,
                                    .clickHz = 800.0,
                                    .clickSeconds = 0.01,
                                    .clickQ = 4.0,
                                    .clickKnock = 1.0,
                                    .length = 0.1,
                                    .drive = 0.5 };
        auto other = knocking;
        other.seed = 2;
        const auto knock = drums::makeKick (rate, knocking);
        CHECK (areSame (knock, drums::makeKick (rate, other)));

        // The band struck once: a ring at its centre.
        CHECK (toneLevel (knock, 800.0, 0.0, 0.01) > 4.0 * toneLevel (knock, 2400.0, 0.0, 0.01));

        knocking.clickKnock = other.clickKnock = 0.0;
        CHECK_FALSE (areSame (drums::makeKick (rate, knocking), drums::makeKick (rate, other)));
    }
}

TEST_CASE ("A hat rings out as it did; with a fade it holds and is cut, with an attack it comes in, and its low cut's knee is its Q's")
{
    const drums::HatShape ringing { .decaySeconds = 1000.0, .length = 0.5, .noise = 1.0 };
    const auto open = drums::makeHat (rate, ringing);
    CHECK (level (open, 0.4, 0.5) == doctest::Approx (level (open, 0.01, 0.11)).epsilon (0.2)); // never cut

    // A hold without a fade is no cut.
    auto unfaded = ringing;
    unfaded.holdSeconds = 0.2;
    CHECK (areSame (open, drums::makeHat (rate, unfaded)));

    auto cut = ringing;
    cut.holdSeconds = 0.2;
    cut.fadeSeconds = 0.05;
    const auto held = drums::makeHat (rate, cut);
    CHECK (areSame (partOf (open, at (0.2)), partOf (held, at (0.2))));                            // as it was, until then
    CHECK (std::abs (decibels (level (held, 0.22, 0.23) / level (open, 0.22, 0.23)) + 6.0) < 1.0); // halfway down, halfway through
    CHECK (held.getMagnitude (0, at (0.25) + 1, held.getNumSamples() - at (0.25) - 1) == 0.0f);    // then nothing

    // Still falling by its decay while it holds: 15 dB down where 1.75 of its time constants have gone.
    cut.decaySeconds = 0.1;
    const auto falling = drums::makeHat (rate, cut);
    CHECK (std::abs (decibels (level (falling, 0.15, 0.2) / level (open, 0.15, 0.2)) + 15.2) < 1.0);

    auto soft = ringing;
    soft.attackSeconds = 0.01;
    const auto pedal = drums::makeHat (rate, soft);
    CHECK (level (pedal, 0.0, 0.002) < 0.3 * level (open, 0.0, 0.002));
    CHECK (level (pedal, 0.02, 0.1) == doctest::Approx (level (open, 0.02, 0.1)).epsilon (0.001));

    // At its low cut, a knee of Q 0.5 is 6 dB down where one of 0.707 is 3.
    auto gentle = ringing;
    gentle.lowCutQ = 0.5;
    const auto knee = decibels (level (bandOf (drums::makeHat (rate, gentle), 5700.0, 6300.0), 0.05, 0.5)
                                / level (bandOf (open, 5700.0, 6300.0), 0.05, 0.5));
    CHECK (knee < -1.5);
    CHECK (knee > -4.5);
}

TEST_CASE ("A clap strikes as often as it says, and with a width its sides are a little apart")
{
    const drums::ClapShape plain { .tailSeconds = 0.01, .length = 0.3, .tailLevel = 0.0 };
    const auto four = drums::makeClap (rate, plain);
    CHECK (four.getNumChannels() == 1);

    auto more = plain;
    more.strikes = 5;
    const auto five = drums::makeClap (rate, more);

    // The same hands until the fifth strikes, 37.5 ms in; then a burst where four have died away.
    CHECK (areSame (partOf (four, at (0.031)), partOf (five, at (0.031))));
    CHECK (level (five, 0.0375, 0.0395) > 3.0 * level (four, 0.0375, 0.0395));

    more.strikes = 1;
    CHECK (level (drums::makeClap (rate, more), 0.0105, 0.0125) < 0.2 * level (four, 0.0105, 0.0125));

    // Each side's own noise beside what they share: a little, 14 dB under it; more, wider; and still the same clap.
    auto apart = plain;
    apart.width = 0.3;
    const auto wide = drums::makeClap (rate, apart);
    REQUIRE (wide.getNumChannels() == 2);
    CHECK (std::abs (sideAgainstMid (wide, 0.0, 0.04) + 13.7) < 2.5);
    CHECK (correlation (wide, 0.0, 0.04) > 0.85);
    apart.width = 0.8;
    CHECK (sideAgainstMid (drums::makeClap (rate, apart), 0.0, 0.04) > sideAgainstMid (wide, 0.0, 0.04) + 5.0);
    CHECK (level (wide, 0.0, 0.04) == doctest::Approx (level (four, 0.0, 0.04)).epsilon (0.15));
    CHECK (areSame (wide, drums::makeClap (rate, { .tailSeconds = 0.01, .length = 0.3, .tailLevel = 0.0, .width = 0.3 })));
}

TEST_CASE ("What is struck rings at its partials, each held as long as it says, under a click that holds and then falls")
{
    // Two partials and no click: they sound, and nothing between them.
    const auto bell = drums::makeStruck (
        rate, { .partials = { { .hz = 600.0, .tau = 0.05 }, { .hz = 1500.0, .tau = 0.03, .level = 0.5 } }, .click = 0.0, .length = 0.3 });
    CHECK (bell.getNumChannels() == 1);
    CHECK (toneLevel (bell, 600.0, 0.0, 0.1) > 10.0 * toneLevel (bell, 1000.0, 0.0, 0.1));
    CHECK (toneLevel (bell, 1500.0, 0.0, 0.1) > 5.0 * toneLevel (bell, 1000.0, 0.0, 0.1));
    CHECK (toneLevel (bell, 600.0, 0.0, 0.1) > 1.5 * toneLevel (bell, 1500.0, 0.0, 0.1)); // the louder and longer one

    // A partial held 20 ms, then gone in a few.
    const auto held =
        drums::makeStruck (rate, { .partials = { { .hz = 1000.0, .tau = 0.003, .hold = 0.02 } }, .click = 0.0, .length = 0.1 });
    CHECK (level (held, 0.012, 0.018) == doctest::Approx (level (held, 0.002, 0.008)).epsilon (0.02));
    CHECK (decibels (level (held, 0.035, 0.04) / level (held, 0.002, 0.008)) < -30.0);

    // The click alone: held, then falling; under its high cut; from its seed.
    const drums::StruckShape stick { .click = 1.0,
                                     .clickHz = 3000.0,
                                     .clickHighCut = 6000.0,
                                     .clickHoldSeconds = 0.015,
                                     .clickSeconds = 0.002,
                                     .length = 0.1 };
    const auto click = drums::makeStruck (rate, stick);
    CHECK (std::abs (decibels (level (click, 0.008, 0.015) / level (click, 0.001, 0.008))) < 5.0);
    CHECK (decibels (level (click, 0.035, 0.045) / level (click, 0.001, 0.015)) < -30.0);

    auto open = stick;
    open.clickHighCut = 20000.0;
    const auto bright = drums::makeStruck (rate, open);
    CHECK (decibels (level (bandOf (click, 10000.0, 16000.0), 0.0, 0.02) / level (bandOf (bright, 10000.0, 16000.0), 0.0, 0.02)) < -15.0);
    CHECK (decibels (level (bandOf (click, 2000.0, 4000.0), 0.0, 0.02) / level (bandOf (bright, 2000.0, 4000.0), 0.0, 0.02)) > -3.0);

    CHECK (areSame (click, drums::makeStruck (rate, stick)));
    auto other = stick;
    other.seed = 2;
    CHECK_FALSE (areSame (click, drums::makeStruck (rate, other)));

    // Saturated only with a drive.
    auto driven = stick;
    driven.partials = { { .hz = 500.0, .tau = 0.05, .level = 2.0 } };
    driven.click = 0.0;
    CHECK (drums::makeStruck (rate, driven).getMagnitude (0, at (0.1)) > 1.5f);
    driven.drive = 2.0;
    CHECK (drums::makeStruck (rate, driven).getMagnitude (0, at (0.1)) <= 1.05f);
}

TEST_CASE ("A room answers after its pre-delay and falls smoothly, its top sooner, each side its own")
{
    // A click played into it.
    Sound click (1, at (0.01));
    click.clear();
    click.setSample (0, 0, 1.0f);
    const drums::RoomShape shape { .preDelaySeconds = 0.02,
                                   .onsetSeconds = 0.01,
                                   .decaySeconds = 0.6,
                                   .highDecay = 0.4,
                                   .dampingHz = 4000.0,
                                   .level = 0.5,
                                   .lowCut = 100.0,
                                   .highCut = 12000.0 };
    const auto room = drums::addRoom (click, rate, shape);
    REQUIRE (room.getNumChannels() == 2);
    CHECK (room.getNumSamples() >= at ((0.02 + 0.6) / 0.7) - 1); // its tail 60 dB down before a kit's fade

    // The click as it was, nothing more until the pre-delay, then the room at its level against it.
    CHECK (room.getSample (0, 0) == doctest::Approx (1.0f));
    CHECK (room.getMagnitude (1, at (0.02) - 2) < 1.0e-4f);
    CHECK (room.getMagnitude (at (0.02), room.getNumSamples() - at (0.02)) == doctest::Approx (0.5f).epsilon (0.01));

    // Coming in over its onset: quieter in its first half than just after.
    CHECK (level (room, 0.02, 0.025) < 0.6 * level (room, 0.03, 0.035));

    // Its low end falls 60 dB in 0.6 s, a little faster by 250 Hz; its top more than half again as fast.
    const auto low = bandOf (room, 100.0, 400.0), high = bandOf (room, 6000.0, 12000.0);
    const auto fallOf = [] (const Sound& band, double start, double end, double window)
    { return (decibels (levelOfBoth (band, start, start + window)) - decibels (levelOfBoth (band, end, end + window))) / (end - start); };
    const auto lowFall = fallOf (low, 0.1, 0.35, 0.1), highFall = fallOf (high, 0.05, 0.2, 0.05);
    CHECK (lowFall > 85.0);
    CHECK (lowFall < 135.0);
    CHECK (highFall > 160.0);
    CHECK (highFall < 235.0);

    // Smoothly: each stretch of it quieter than the one before, where a gate's hold would be flat and its close a cliff.
    for (auto from = 0.05; from < 0.5; from += 0.05)
    {
        CAPTURE (from);
        const auto step = decibels (levelOfBoth (room, from + 0.05, from + 0.1)) - decibels (levelOfBoth (room, from, from + 0.05));
        CHECK (step < 0.0);
        CHECK (step > -14.0);
    }

    // Each side a room of its own; narrower, more alike; at no width, one room.
    CHECK (std::abs (correlation (room, 0.05, 0.4)) < 0.2);
    auto narrow = shape;
    narrow.width = 0.3;
    CHECK (correlation (drums::addRoom (click, rate, narrow), 0.05, 0.4) > 0.5);
    narrow.width = 0.0;
    const auto mono = drums::addRoom (click, rate, narrow);
    CHECK (areSame (partOf (mono, mono.getNumSamples(), 0), partOf (mono, mono.getNumSamples(), 1)));

    // A top that takes as long as the low end falls as it does.
    auto even = shape;
    even.highDecay = 1.0;
    const auto evenRoom = drums::addRoom (click, rate, even);
    CHECK (fallOf (bandOf (evenRoom, 6000.0, 12000.0), 0.05, 0.2, 0.05) == doctest::Approx (100.0).epsilon (0.15));

    // Its band: little above its high cut.
    auto dark = shape;
    dark.highCut = 2000.0;
    const auto darkRoom = drums::addRoom (click, rate, dark);
    CHECK (decibels (level (bandOf (darkRoom, 6000.0, 12000.0), 0.03, 0.1) / level (bandOf (darkRoom, 500.0, 1500.0), 0.03, 0.1))
           < decibels (level (high, 0.03, 0.1) / level (bandOf (room, 500.0, 1500.0), 0.03, 0.1)) - 12.0);

    // From its seed.
    CHECK (areSame (room, drums::addRoom (click, rate, shape)));
    auto other = shape;
    other.seed = 2;
    CHECK_FALSE (areSame (room, drums::addRoom (click, rate, other)));

    // A longer drum keeps its length, and a stereo one its sides.
    const auto snare =
        drums::makeStereo (drums::makeSnare (rate, { .length = 2.0 }), drums::makeSnare (rate, { .length = 2.0, .seed = 9 }));
    const auto wet = drums::addRoom (snare, rate, shape);
    CHECK (wet.getNumSamples() == snare.getNumSamples());
    CHECK (wet.getSample (0, 5) == doctest::Approx (snare.getSample (0, 5)));
    CHECK (wet.getSample (1, 5) == doctest::Approx (snare.getSample (1, 5)));
}

TEST_CASE ("Made at another rate, a room, what is struck and a held hat keep their shapes in time")
{
    // At 44.1 and 96 kHz as at 48 kHz: what is struck as long, its hold and its fall; a room's tail falling as fast; a hat
    // held and cut at the same times. (Their noise is drawn a sample at a time, so other noise, and its level in a band
    // follows the rate, as all of these drums' noise does: the kits are made at 48 kHz.)
    const auto levelAt = [] (const Sound& sound, double sampleRate, double from, double to)
    {
        auto sum = 0.0;
        const auto start = (int) (from * sampleRate), end = std::min ((int) (to * sampleRate), sound.getNumSamples());

        for (int channel = 0; channel < sound.getNumChannels(); ++channel)
            for (int i = start; i < end; ++i)
                sum += (double) sound.getSample (channel, i) * sound.getSample (channel, i);

        return decibels (std::sqrt (sum / std::max (1, (end - start) * sound.getNumChannels())));
    };

    struct Made
    {
        Sound struck, room, hat, uncut;
    };

    const auto make = [] (double sampleRate)
    {
        Sound click (1, 16);
        click.clear();
        click.setSample (0, 0, 1.0f);
        const drums::HatShape hat { .decaySeconds = 1000.0, .length = 0.3, .holdSeconds = 0.2, .fadeSeconds = 0.05 };
        auto uncut = hat;
        uncut.fadeSeconds = 0.0;
        const drums::StruckShape struck { .partials = { { .hz = 900.0, .tau = 0.02, .hold = 0.01 }, { .hz = 2300.0, .tau = 0.01 } },
                                          .click = 0.0,
                                          .length = 0.2 };
        return Made { drums::makeStruck (sampleRate, struck),
                      drums::addRoom (click, sampleRate, { .preDelaySeconds = 0.01, .decaySeconds = 0.5 }),
                      drums::makeHat (sampleRate, hat), drums::makeHat (sampleRate, uncut) };
    };

    const auto reference = make (rate);

    for (const auto sampleRate : { 44100.0, 96000.0 })
    {
        CAPTURE (sampleRate);
        const auto other = make (sampleRate);
        CHECK (other.struck.getNumSamples() == (int) (0.2 * sampleRate));
        CHECK (other.room.getNumSamples() == doctest::Approx (reference.room.getNumSamples() * sampleRate / rate).epsilon (0.001));

        // What is struck: each stretch against its first 10 ms, the same.
        const auto shape = [&] (const Sound& sound, double soundRate, double from, double to)
        { return levelAt (sound, soundRate, from, to) - levelAt (sound, soundRate, 0.0, 0.01); };

        for (const auto from : { 0.005, 0.02, 0.05 })
            CHECK (std::abs (shape (other.struck, sampleRate, from, from + 0.005) - shape (reference.struck, rate, from, from + 0.005))
                   < 0.5);

        // A room's answer to a click: falling as far between the same times.
        const auto fall = [&] (const Sound& sound, double soundRate)
        { return levelAt (sound, soundRate, 0.05, 0.15) - levelAt (sound, soundRate, 0.25, 0.35); };

        CHECK (std::abs (fall (other.room, sampleRate) - fall (reference.room, rate)) < 2.0);

        // The hat against itself uncut: only the cut's gain, whatever the metal and the noise.
        const auto cut = [&] (const Made& made, double soundRate, double from)
        { return levelAt (made.hat, soundRate, from, from + 0.01) - levelAt (made.uncut, soundRate, from, from + 0.01); };

        for (const auto from : { 0.1, 0.19, 0.21, 0.23 })
            CHECK (std::abs (cut (other, sampleRate, from) - cut (reference, rate, from)) < 0.5);

        CHECK (other.hat.getMagnitude (0, (int) (0.25 * sampleRate) + 1, other.hat.getNumSamples() - (int) (0.25 * sampleRate) - 1)
               == 0.0f);
    }
}

TEST_CASE ("A snare's further modes ring in its shell, and a Simmons drum's tone holds for its hold")
{
    const auto snare = drums::makeSnare (rate);
    const auto ringing = drums::makeSnare (rate, { .modes = { { .hz = 520.0, .tau = 0.05, .level = 0.3 } } });
    CHECK (toneLevel (ringing, 520.0, 0.0, 0.1) > 3.0 * toneLevel (snare, 520.0, 0.0, 0.1));
    CHECK (toneLevel (ringing, 185.0, 0.0, 0.1) == doctest::Approx (toneLevel (snare, 185.0, 0.0, 0.1)).epsilon (0.1));

    const drums::SimmonsShape
        tone { .startHz = 150.0, .endHz = 150.0, .toneSeconds = 0.05, .noise = 0.0, .click = 0.0, .length = 0.6, .drive = 0.5 };
    auto held = tone;
    held.holdSeconds = 0.3;
    const auto fading = drums::makeSimmons (rate, tone), holding = drums::makeSimmons (rate, held);
    CHECK (decibels (level (fading, 0.25, 0.3) / level (fading, 0.01, 0.06)) < -30.0);
    CHECK (std::abs (decibels (level (holding, 0.25, 0.3) / level (holding, 0.01, 0.06))) < 1.0);
    CHECK (decibels (level (holding, 0.5, 0.55) / level (holding, 0.01, 0.06)) < -30.0);
}

TEST_CASE ("The sounds that were there before the natural kit's shapes are as they were")
{
    // Each one's length and level (the root mean square of its left side, its peak at -6 dB), as the code before the
    // shapes grew made them, through each function that grew: a kick's shape, a snare's, a clap's, a hat's, a Simmons
    // drum's, the gated room, and the drum machines' rings.
    struct Before
    {
        const char* name;
        int samples;
        double rms;
    };

    for (const auto& before : std::initializer_list<Before> { { "Synthwave Kick", 21600, 0.1306559 },
                                                              { "Synthwave Snare", 23040, 0.08448911 },
                                                              { "Synthwave Clap", 15655, 0.06891617 },
                                                              { "Synthwave Low Tom", 57600, 0.08651727 },
                                                              { "House Closed Hat", 8640, 0.05526841 },
                                                              { "House Clap", 38400, 0.05682934 },
                                                              { "Punchy Snare", 16800, 0.1580525 },
                                                              { "808 Snare", 14400, 0.05167243 },
                                                              { "909 Rim", 2880, 0.06805827 },
                                                              { "Percussion Low Agogo", 48000, 0.06820167 } })
    {
        CAPTURE (std::string (before.name));
        const auto sound = getDrum (before.name);
        CHECK (sound->audio.getNumSamples() == before.samples);
        CHECK (level (sound->audio, 0.0, 100.0) == doctest::Approx (before.rms).epsilon (0.0001));
    }
}

TEST_CASE ("The Dark Synthwave Kit's sixteen sounds: natural drums, the same every time they are made")
{
    static constexpr std::array<const char*, 16> names {
        "Dark Synthwave Kick",      "Dark Synthwave Perc",    "Dark Synthwave Snare",      "Dark Synthwave Clap",
        "Dark Synthwave Snare 2",   "Dark Synthwave Low Tom", "Dark Synthwave Closed Hat", "Dark Synthwave Floor Tom",
        "Dark Synthwave Pedal Hat", "Dark Synthwave Mid Tom", "Dark Synthwave Open Hat",   "Dark Synthwave Mid Tom 2",
        "Dark Synthwave High Tom",  "Dark Synthwave Crash",   "Dark Synthwave High Tom 2", "Dark Synthwave Ride"
    };
    const auto first = devices::findFactoryDrum (names[0]);
    REQUIRE (first >= devices::DrumSampler::kitPads);
    CHECK (first % devices::DrumSampler::kitPads == 0);

    for (size_t pad = 0; pad < names.size(); ++pad)
    {
        CAPTURE (std::string (names[pad]));
        REQUIRE (devices::findFactoryDrum (names[pad]) == first + (int) pad); // in the pads' order, from the kick

        // Made again once nothing holds it: sample for sample the same.
        const auto made = devices::getFactoryDrum (first + (int) pad)->audio;
        const auto again = devices::getFactoryDrum (first + (int) pad);
        CHECK (areSame (made, again->audio));

        auto finite = true;

        for (int channel = 0; channel < made.getNumChannels(); ++channel)
            for (int i = 0; i < made.getNumSamples(); ++i)
                finite = finite && std::isfinite (made.getSample (channel, i));

        CHECK (finite);
        CHECK (made.getMagnitude (0, made.getNumSamples()) < 0.502f); // under full scale: -6 dB
        CHECK (made.getMagnitude (made.getNumSamples() - 1, 1) < 1.0e-6f);

        // Ending in silence: its last tenth 40 dB under its loudest 10 ms.
        auto loudest = 0.0;

        for (auto from = 0.0; from < made.getNumSamples() / rate - 0.01; from += 0.005)
            loudest = std::max (loudest, level (made, from, from + 0.01));

        CHECK (decibels (level (made, 0.9 * made.getNumSamples() / rate, made.getNumSamples() / rate) / loudest) < -40.0);
    }

    // The kick: mono, short and held, gone within a fifth of a second.
    const auto kick = getDrum (names[0]);
    CHECK (kick->audio.getNumChannels() == 1);
    CHECK (decibels (level (kick->audio, 0.045, 0.05) / level (kick->audio, 0.025, 0.03)) > -6.0);
    CHECK (decibels (level (kick->audio, 0.15, 0.2) / level (kick->audio, 0.025, 0.03)) < -40.0);

    // The snares, the clap and the perc: the hit in the middle (the clap's hands a little apart), then a room that spreads
    // wide and isn't gated.
    for (const auto pad : { 1, 2, 3, 4 })
    {
        CAPTURE (std::string (names[(size_t) pad]));
        const auto drum = getDrum (names[(size_t) pad]);
        REQUIRE (drum->audio.getNumChannels() == 2);
        CHECK (correlation (drum->audio, 0.0, 0.015) > (pad == 3 ? 0.75 : 0.95));
        CHECK (std::abs (correlation (drum->audio, 0.08, 0.2)) < 0.5);
    }

    const auto snare = getDrum (names[2]);
    const auto fall = [&] (double from)
    { return decibels (level (snare->audio, from, from + 0.1, 0) / level (snare->audio, from + 0.1, from + 0.2, 0)); };
    CHECK (fall (0.1) > 3.0); // falling all the way: no gate's hold
    CHECK (fall (0.2) > 3.0);
    CHECK (fall (0.3) > 3.0);
    CHECK (fall (0.3) < 20.0); // and no gate's cliff

    // The open hat: held, then cut; the closed one short.
    const auto openHat = getDrum (names[10]), closedHat = getDrum (names[6]);
    CHECK (decibels (level (openHat->audio, 0.2, 0.25) / level (openHat->audio, 0.0, 0.05)) > -10.0);
    CHECK (decibels (level (openHat->audio, 0.33, 0.45) / level (openHat->audio, 0.0, 0.05)) < -50.0);
    CHECK (decibels (level (closedHat->audio, 0.15, 0.2) / level (closedHat->audio, 0.0, 0.05)) < -25.0);

    // The toms, from the lowest up.
    auto last = 0;

    for (const auto pad : { 5, 7, 9, 11, 12, 14 })
    {
        CAPTURE (std::string (names[(size_t) pad]));
        const auto crossings = countCrossings (getDrum (names[(size_t) pad])->audio, 0.02, 0.12);
        CHECK (crossings > last);
        last = crossings;
    }
}
