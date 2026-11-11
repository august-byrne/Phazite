// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's multiband (src/devices/dynamics/Multiband.h), in Maximus's image: its description, its curves' axis and shapes,
// what it does to quiet and loud sound, its band modes and solo, LMH Mix, the master band driven by the mid band, its
// saturation, stereo separation and releases; and its sidechain: the bands whose Key is on ducking under the key, through
// its cuts, and Listen.
#include "TestHelpers.h"
#include "devices/dynamics/Multiband.h"

#include <cmath>
#include <functional>
#include <vector>

namespace
{
namespace devices = phazite::devices;
using devices::Multiband;
namespace curves = devices::curves;
constexpr double testRate = 48000.0;
constexpr int testBlockSize = 480;
constexpr double twoPi = juce::MathConstants<double>::twoPi;

std::vector<float> getDefaults()
{
    std::vector<float> values;

    for (const auto& parameter : Multiband::info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

void set (std::vector<float>& values, std::string_view id, float value)
{
    const auto index = devices::findParameter (Multiband::info, id);
    REQUIRE (index >= 0);
    values[(size_t) index] = value;
}

// A stereo sound: its two channels.
struct Stereo
{
    std::vector<float> left, right;
};

// Plays the multiband in blocks of 10 ms on a sound (input gives each channel's sample at a time), with a key on its sidechain
// if one is given (as input gives its), and returns what comes out.
template <typename Input>
Stereo play (Multiband& multiband, const std::vector<float>& values, int numSamples, Input input,
             const std::function<std::pair<float, float> (int)>& key = {})
{
    juce::AudioBuffer<float> buffer (2, testBlockSize), keyBuffer (2, testBlockSize);
    Stereo played;

    for (int done = 0; done < numSamples; done += testBlockSize)
    {
        const auto length = std::min (testBlockSize, numSamples - done);

        for (int i = 0; i < length; ++i)
        {
            const auto [left, right] = input (done + i);
            buffer.setSample (0, i, left);
            buffer.setSample (1, i, right);

            if (key)
            {
                const auto [keyLeft, keyRight] = key (done + i);
                keyBuffer.setSample (0, i, keyLeft);
                keyBuffer.setSample (1, i, keyRight);
            }
        }

        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        const float* keyChannels[] = { keyBuffer.getReadPointer (0), keyBuffer.getReadPointer (1) };
        devices::ProcessContext context;
        context.channels = channels;
        context.numChannels = 2;
        context.numSamples = length;
        context.parameters = values;

        if (key)
        {
            context.sidechain = keyChannels;
            context.numSidechainChannels = 2;
        }

        multiband.process (context);

        for (int i = 0; i < length; ++i)
        {
            played.left.push_back (buffer.getSample (0, i));
            played.right.push_back (buffer.getSample (1, i));
        }
    }

    return played;
}

auto sine (double frequency, float decibels)
{
    const auto amplitude = juce::Decibels::decibelsToGain (decibels);
    return [frequency, amplitude] (int t)
    {
        const auto value = amplitude * (float) std::sin (twoPi * frequency * t / testRate);
        return std::pair { value, value };
    };
}

// A sine's level from its RMS, in decibels of its peak, over the second half of a sound.
float getLevel (const std::vector<float>& sound)
{
    auto sum = 0.0;

    for (auto i = sound.size() / 2; i < sound.size(); ++i)
        sum += (double) sound[i] * sound[i];

    return 20.0f * std::log10 (std::max (1.0e-9f, (float) std::sqrt (2.0 * sum / (double) (sound.size() - sound.size() / 2))));
}

float getPeak (const std::vector<float>& sound, size_t from)
{
    auto peak = 0.0f;

    for (auto i = from; i < sound.size(); ++i)
        peak = std::max (peak, std::abs (sound[i]));

    return peak;
}

// The level a sine at a frequency and level comes out at, for some parameters.
float levelOut (const std::vector<float>& values, double frequency, float decibels)
{
    Multiband multiband;
    multiband.prepare (testRate, testBlockSize);
    return getLevel (play (multiband, values, 48000, sine (frequency, decibels)).left);
}

// A band's curve set to limit at a level, as a flat line from it to the end.
void limitAt (std::vector<float>& values, const char* band, float decibels)
{
    const auto place = curves::axisForDecibels (decibels);
    const auto prefix = juce::String (band);
    set (values, (prefix + "Point2X").toStdString(), place);
    set (values, (prefix + "Point2Y").toStdString(), place);
    set (values, (prefix + "Point3Y").toStdString(), place);
}
} // namespace

TEST_CASE ("The multiband describes itself: global parameters, then each band's, its curve's points last")
{
    REQUIRE (devices::isValid (Multiband::info));
    CHECK (Multiband::info.parameters.size() == (size_t) Multiband::getBandParameter (Multiband::numBands, Multiband::mode));
    CHECK (devices::findParameter (Multiband::info, "lmhMix") == Multiband::lmhMix);
    CHECK (devices::findParameter (Multiband::info, "midPre") == Multiband::getBandParameter (Multiband::mid, Multiband::pre));
    CHECK (devices::findParameter (Multiband::info, "masterRelease2")
           == Multiband::getBandParameter (Multiband::master, Multiband::release2));
    CHECK (devices::findParameter (Multiband::info, "highPoint8Type")
           == Multiband::getPointParameter (Multiband::high, curves::maxPoints - 1, curves::pointType));
    CHECK (devices::findParameter (Multiband::info, devices::mixParameterID) < 0); // LMH Mix, before the master band, is its mix

    // A solo for the low, mid and high bands, and none for the master, which did nothing; a Key for each of the four, and the
    // key's cuts, for its sidechain.
    CHECK (devices::findParameter (Multiband::info, "lowSolo") == Multiband::getSoloParameter (Multiband::low));
    CHECK (devices::findParameter (Multiband::info, "highSolo") == Multiband::getSoloParameter (Multiband::high));
    CHECK (devices::findParameter (Multiband::info, "masterSolo") < 0);
    CHECK (devices::findParameter (Multiband::info, "lowKey") == Multiband::getBandParameter (Multiband::low, Multiband::key));
    CHECK (devices::findParameter (Multiband::info, "masterKey") == Multiband::getBandParameter (Multiband::master, Multiband::key));
    CHECK (devices::findParameter (Multiband::info, "keyLowCut") == Multiband::keyLowCut);
    CHECK (devices::findParameter (Multiband::info, "keyHighCut") == Multiband::keyHighCut);
    CHECK (Multiband::info.hasSidechain);

    // Its latency is the most both look-aheads take, whatever they are set to, and the two saturations' at twice the rate.
    Multiband multiband;
    multiband.prepare (testRate, testBlockSize);
    const auto saturation = (int) std::lround (phazite::devices::dsp::AntialiasedShaper::getLatencySamples());
    CHECK (saturation > 0);
    CHECK (saturation < 8);
    CHECK (multiband.getLatencySamples() == 2 * (int) std::lround (Multiband::mostLookaheadSeconds * testRate) + 2 * saturation);
}

TEST_CASE ("The multiband's curves: Maximus's axis, a limiter at first, and each line's shape")
{
    // Silence at one end, 0 dB in the middle, +12 dB at the other.
    CHECK (curves::toAxis (0.0f) == doctest::Approx (0.0f));
    CHECK (curves::toAxis (1.0f) == doctest::Approx (0.5f));
    CHECK (curves::toAxis (4.0f) == doctest::Approx (1.0f));
    CHECK (curves::fromAxis (curves::toAxis (0.3f)) == doctest::Approx (0.3f));
    CHECK (curves::axisForDecibels (0.0f) == doctest::Approx (0.5f));

    // At first: level in, level out, up to 0 dB; then 0 dB.
    auto values = getDefaults();
    CHECK (curves::getAt (Multiband::getCurve (values, Multiband::low), 0.3f) == doctest::Approx (0.3f));
    CHECK (curves::getAt (Multiband::getCurve (values, Multiband::low), 0.8f) == doctest::Approx (0.5f));

    // A tension above 0 bends the line before a point up; a hold keeps the level before it; stairs step.
    set (values, "lowPoint2Tension", 0.5f);
    CHECK (curves::getAt (Multiband::getCurve (values, Multiband::low), 0.25f) > 0.3f);
    set (values, "lowPoint2Type", (float) curves::PointType::hold);
    CHECK (curves::getAt (Multiband::getCurve (values, Multiband::low), 0.25f) == doctest::Approx (0.0f));
    set (values, "lowPoint2Type", (float) curves::PointType::stairs);
    set (values, "lowPoint2Tension", -1.0f); // the fewest steps: two
    CHECK (curves::getStairs (-1.0f) == 2);
    CHECK (curves::getAt (Multiband::getCurve (values, Multiband::low), 0.2f) == doctest::Approx (0.0f));
    CHECK (curves::getAt (Multiband::getCurve (values, Multiband::low), 0.3f) == doctest::Approx (0.5f));

    // A double curve passes through the middle of its line, as an S.
    set (values, "lowPoint2Type", (float) curves::PointType::doubleCurve);
    set (values, "lowPoint2Tension", 0.8f);
    CHECK (curves::getAt (Multiband::getCurve (values, Multiband::low), 0.25f) == doctest::Approx (0.25f).epsilon (0.01));
    CHECK (curves::getAt (Multiband::getCurve (values, Multiband::low), 0.1f) < 0.1f);
    CHECK (curves::getAt (Multiband::getCurve (values, Multiband::low), 0.4f) > 0.4f);
}

TEST_CASE ("The multiband passes quiet sound as it came in, at any frequency, and limits loud sound at 0 dB")
{
    const auto values = getDefaults();

    for (const auto frequency : { 50.0, 120.0, 1000.0, 5000.0, 12000.0 })
    {
        INFO (frequency);
        CHECK (levelOut (values, frequency, -20.0f) == doctest::Approx (-20.0f).epsilon (0.02));
    }

    // 6 dB over: each band and the master limit at 0 dB.
    Multiband multiband;
    multiband.prepare (testRate, testBlockSize);
    const auto loud = play (multiband, values, 48000, sine (1000.0, 6.0f));
    CHECK (getLevel (loud.left) == doctest::Approx (0.0f).epsilon (0.5));
    CHECK (getPeak (loud.left, 24000) < juce::Decibels::decibelsToGain (1.0f));
}

TEST_CASE ("The multiband's bands: muted, off (the mid band taking its room) and soloed")
{
    auto values = getDefaults();

    // Muting the low band takes the bass away, and leaves the middle.
    set (values, "lowMode", (float) Multiband::Mode::muted);
    CHECK (levelOut (values, 50.0, -20.0f) < -45.0f);
    CHECK (levelOut (values, 1000.0, -20.0f) == doctest::Approx (-20.0f).epsilon (0.05));

    // With the low band off, the mid band covers the bass: its pre gain raises it.
    values = getDefaults();
    set (values, "midPre", 6.0f);
    CHECK (levelOut (values, 50.0, -20.0f) == doctest::Approx (-20.0f).epsilon (0.05));
    set (values, "lowMode", (float) Multiband::Mode::off);
    CHECK (levelOut (values, 50.0, -20.0f) == doctest::Approx (-14.0f).epsilon (0.05));

    // Soloing the high band leaves the bass out.
    values = getDefaults();
    set (values, "highSolo", 1.0f);
    CHECK (levelOut (values, 50.0, -20.0f) < -45.0f);
    CHECK (levelOut (values, 12000.0, -20.0f) == doctest::Approx (-20.0f).epsilon (0.05));
}

TEST_CASE ("The multiband's LMH Mix, its master band driven by the mid band, its saturation and stereo separation")
{
    // LMH Mix at 0: the bands' gains no longer reach what comes out; the master's still do.
    auto values = getDefaults();
    set (values, "lowPre", 12.0f);
    set (values, "midPre", 12.0f);
    set (values, "highPre", 12.0f);
    set (values, "lmhMix", 0.0f);
    CHECK (levelOut (values, 1000.0, -20.0f) == doctest::Approx (-20.0f).epsilon (0.05));
    set (values, "masterPost", -6.0f);
    CHECK (levelOut (values, 1000.0, -20.0f) == doctest::Approx (-26.0f).epsilon (0.05));

    // The master limiting at -30 dB turns a bass note down, unless the mid band, silent, drives it.
    values = getDefaults();
    limitAt (values, "master", -30.0f);
    CHECK (levelOut (values, 50.0, -10.0f) < -25.0f);
    set (values, "midDrivesMaster", 1.0f);
    CHECK (levelOut (values, 50.0, -10.0f) == doctest::Approx (-10.0f).epsilon (0.05));

    // Saturation: turned all the way, the master's rounds a loud sine's peaks off under its ceiling, -6 dB.
    values = getDefaults();
    limitAt (values, "master", 12.0f); // no limiting: the curve straight to the end
    limitAt (values, "low", 12.0f);
    limitAt (values, "mid", 12.0f);
    limitAt (values, "high", 12.0f);
    set (values, "masterSaturation", -1.0f);
    set (values, "masterCeiling", -6.0f);
    Multiband multiband;
    multiband.prepare (testRate, testBlockSize);
    const auto saturated = play (multiband, values, 48000, sine (1000.0, -3.0f));
    CHECK (getPeak (saturated.left, 24000) <= juce::Decibels::decibelsToGain (-6.0f) * 1.001f);
    CHECK (getPeak (saturated.left, 24000) > juce::Decibels::decibelsToGain (-12.0f));

    // Stereo separation at -1: mono, a sound in the left channel only coming out on both alike.
    values = getDefaults();
    set (values, "masterStereo", -1.0f);
    multiband.reset();
    const auto left = sine (1000.0, -20.0f);
    const auto mono = play (multiband, values, 24000, [&left] (int t) { return std::pair { left (t).first, 0.0f }; });
    CHECK (getLevel (mono.left) == doctest::Approx (getLevel (mono.right)).epsilon (0.01));
    CHECK (getLevel (mono.left) == doctest::Approx (-26.0f).epsilon (0.05)); // half of it each side
}

TEST_CASE ("The multiband's LMH Mix meets the bands in phase: half and half, a tone at either crossover keeps its level")
{
    // The bands add up to the sound through an all-pass at each crossover, half a turn there at 24 dB: had the dry sound not
    // gone through the same, half of each would cancel out there.
    for (const auto slope : { 0.0f, 1.0f })
    {
        auto values = getDefaults();
        set (values, "lmhMix", 0.5f);
        set (values, "lowSlope", slope);
        set (values, "highSlope", slope);

        for (const auto frequency : { 120.0, 1000.0, 5000.0 })
        {
            INFO (slope << " " << frequency);
            CHECK (levelOut (values, frequency, -20.0f) == doctest::Approx (-20.0f).epsilon (0.01));
        }
    }
}

TEST_CASE ("The multiband's releases: the first speeds up and is done in its time, the second slows into the gain")
{
    // A loud burst into the master's limiter at -20 dB, then quiet: how far down the gain still is a while later.
    const auto reductionAfter = [] (float release, float release2, int samplesAfter, float curve = 1.0f)
    {
        auto values = getDefaults();
        limitAt (values, "master", -20.0f);
        set (values, "masterRelease", release);
        set (values, "masterRelease2", release2);
        set (values, "masterCurve", curve);
        Multiband multiband;
        multiband.prepare (testRate, testBlockSize);
        play (multiband, values, 9600, sine (1000.0, -6.0f));
        const auto quiet = play (multiband, values, samplesAfter + 1200, sine (1000.0, -40.0f));
        auto peak = 0.0f;

        for (auto i = (size_t) samplesAfter; i < (size_t) samplesAfter + 1200; ++i)
            peak = std::max (peak, std::abs (quiet.left[i]));

        return -40.0f - 20.0f * std::log10 (std::max (peak, 1.0e-9f));
    };

    // The first release alone, 100 ms: still down halfway through (it starts slowly), back after it.
    CHECK (reductionAfter (100.0f, 0.0f, 2400) > 3.0f);
    CHECK (reductionAfter (100.0f, 0.0f, 7200) == doctest::Approx (0.0f).epsilon (0.5));

    // The second alone, 100 ms, through one stage (curve 1): further back at the same time, as it starts fast and slows.
    CHECK (reductionAfter (0.0f, 100.0f, 2400, 0.0f) < reductionAfter (100.0f, 0.0f, 2400) - 1.0f);
}

TEST_CASE ("A band whose Key is on ducks under the sidechain's key, through the key's cuts; the others hear their own bands")
{
    // The low and mid bands limit at -30 dB: a quiet bass (40 Hz at -40 dB, so low that the mid band takes next to none of it
    // over the crossover) passes them, and a key louder than that ducks a band that hears it, as far as it is over.
    auto values = getDefaults();
    limitAt (values, "low", -30.0f);
    limitAt (values, "mid", -30.0f);
    const auto bass = sine (40.0, -40.0f), kick = sine (80.0, -6.0f), hats = sine (5000.0, -6.0f);
    const auto keyed = [] (const std::vector<float>& v, auto input, const std::function<std::pair<float, float> (int)>& key)
    {
        Multiband multiband;
        multiband.prepare (testRate, testBlockSize);
        return play (multiband, v, 48000, input, key);
    };

    // With no band's Key on, the key changes nothing at all.
    {
        Multiband alone;
        alone.prepare (testRate, testBlockSize);
        const auto plain = play (alone, values, 48000, bass);
        CHECK (keyed (values, bass, kick).left == plain.left);
    }

    // The low band's Key on: the bass ducks under the kick, about 24 dB; a tone in the mid band, whose Key is off, doesn't.
    set (values, "lowKey", 1.0f);
    CHECK (getLevel (keyed (values, bass, kick).left) < -60.0f);
    CHECK (getLevel (keyed (values, sine (1000.0, -40.0f), kick).left) == doctest::Approx (-40.0f).epsilon (0.02));

    // The key's low cut over the kick takes it away: the bass passes again.
    set (values, "keyLowCut", 500.0f);
    CHECK (getLevel (keyed (values, bass, kick).left) == doctest::Approx (-40.0f).epsilon (0.05));

    // Hats on the key duck the bass too, unless its high cut is under them.
    set (values, "keyLowCut", 20.0f);
    CHECK (getLevel (keyed (values, bass, hats).left) < -60.0f);
    set (values, "keyHighCut", 300.0f);
    CHECK (getLevel (keyed (values, bass, hats).left) == doctest::Approx (-40.0f).epsilon (0.05));

    // The master's Key: it ducks what the bands put out under the key, whatever they hear.
    values = getDefaults();
    limitAt (values, "master", -30.0f);
    set (values, "masterKey", 1.0f);
    CHECK (getLevel (keyed (values, sine (1000.0, -40.0f), kick).left) < -60.0f);
}

TEST_CASE ("With no sidechain feeding it, a band whose Key is on hears what comes in, through the key's cuts")
{
    // The mid band limiting at -30 dB turns a 1 kHz tone at -10 dB down to it, hearing itself or what comes in alike.
    auto values = getDefaults();
    limitAt (values, "mid", -30.0f);
    CHECK (levelOut (values, 1000.0, -10.0f) == doctest::Approx (-30.0f).epsilon (0.05));
    set (values, "midKey", 1.0f);
    CHECK (levelOut (values, 1000.0, -10.0f) == doctest::Approx (-30.0f).epsilon (0.05));

    // Through a high cut far under the tone, it hears nearly nothing: the tone passes.
    set (values, "keyHighCut", 100.0f);
    CHECK (levelOut (values, 1000.0, -10.0f) == doctest::Approx (-10.0f).epsilon (0.05));
}

TEST_CASE ("Listen plays the key, through its cuts, in place of what comes out and as late as it, while it is asked for")
{
    const auto values = getDefaults();
    const auto silence = [] (int) { return std::pair { 0.0f, 0.0f }; };
    Multiband multiband;
    multiband.prepare (testRate, testBlockSize);
    CHECK_FALSE (multiband.isPlayingKey());
    multiband.listenToKey();
    CHECK (multiband.isPlayingKey());

    // A click on the key, in silence: it comes out as late as the multiband's sound does, as loud.
    const auto latency = multiband.getLatencySamples();
    const auto clicked =
        play (multiband, values, 4800, silence, [] (int t) { return t == 0 ? std::pair { 0.5f, 0.5f } : std::pair { 0.0f, 0.0f }; });
    size_t loudest = 0;

    for (size_t i = 0; i < clicked.left.size(); ++i)
        if (std::abs (clicked.left[i]) > std::abs (clicked.left[loudest]))
            loudest = i;

    CHECK (loudest == (size_t) latency);
    CHECK (clicked.left[loudest] == doctest::Approx (0.5f).epsilon (0.02));
    CHECK (multiband.overridesMix());

    // Half a second by the clock after it was last asked for, the sound comes back, and the key no longer plays.
    juce::Thread::sleep (600);
    CHECK_FALSE (multiband.isPlayingKey());
    const auto after = play (multiband, values, 9600, sine (1000.0, -20.0f), sine (80.0, -6.0f));
    CHECK (getLevel (after.left) == doctest::Approx (-20.0f).epsilon (0.02));
    CHECK_FALSE (multiband.overridesMix());

    // Asked for again, it starts from silence, not from the key it heard as it last played it: once it has faded in over its
    // first 5 ms (from the sound's tail, which the multiband still holds), a silent key plays nothing.
    multiband.listenToKey();
    const auto again = play (multiband, values, 4800, silence, silence);
    CHECK (getPeak (again.left, 300) < 1.0e-6f);
}

TEST_CASE ("Every parameter of the multiband but its curves' changes what it plays, in a setting where it matters")
{
    // The master's solo went on 2026-11-10: the multiband never read it. Each band limits at a level of its own, grips and lets
    // go within milliseconds through both releases, at times of its own, hears the key, and is driven by its post gain well
    // into its saturation, under a ceiling; on bursts of three tones, wider on one side, and a key of bursts of its own, so that
    // times, curves, look-aheads and splits all show, and a band off sounds through the mid band's chain unlike its own.
    auto base = getDefaults();
    const std::pair<const char*, float> bands[] = { { "low", -20.0f }, { "mid", -24.0f }, { "high", -16.0f }, { "master", -20.0f } };
    auto attack = 3.0f;

    for (const auto& [band, limit] : bands)
    {
        const auto prefix = std::string (band);
        limitAt (base, band, limit);
        set (base, prefix + "Attack", attack += 2.0f);
        set (base, prefix + "Release", 40.0f);
        set (base, prefix + "Release2", 60.0f);
        set (base, prefix + "Sustain", 10.0f);
        set (base, prefix + "Post", 18.0f);
        set (base, prefix + "Saturation", 0.5f);
        set (base, prefix + "Ceiling", -6.0f);
        set (base, prefix + "Key", 1.0f);
    }

    const auto burst = [] (int t, int period) { return (t / period) % 2 == 0 ? 1.0f : 0.0f; };
    const auto input = [&burst] (int t)
    {
        const auto at = [t] (double frequency) { return (float) std::sin (twoPi * frequency * t / testRate); };
        const auto mix = burst (t, 2400) * (0.5f * at (60.0) + 0.3f * at (1000.0) + 0.2f * at (8000.0));
        return std::pair { mix, 0.6f * mix + 0.2f * burst (t, 1600) * at (3000.0) };
    };
    const auto key = [&burst] (int t)
    {
        const auto value =
            burst (t, 1800)
            * (0.5f * (float) std::sin (twoPi * 100.0 * t / testRate) + 0.3f * (float) std::sin (twoPi * 3000.0 * t / testRate));
        return std::pair { value, value };
    };
    const auto playWith = [&input, &key] (const std::vector<float>& values)
    {
        Multiband multiband;
        multiband.prepare (testRate, testBlockSize);
        return play (multiband, values, 14400, input, key);
    };

    const auto before = playWith (base);

    for (int index = 0; index < (int) Multiband::info.parameters.size(); ++index)
    {
        const auto& parameter = Multiband::info.parameters[(size_t) index];
        const auto id = std::string (parameter.id);

        if (id.find ("Point") != std::string::npos) // the curves' own, which their shapes test
            continue;

        CAPTURE (id);
        auto values = base;
        auto& value = values[(size_t) index];
        value = parameter.unit == devices::Unit::choice || parameter.unit == devices::Unit::toggle
                    ? (value + 1.0f > parameter.maximum ? parameter.minimum : parameter.maximum)
                    : (juce::approximatelyEqual (value, parameter.maximum) ? parameter.minimum : parameter.maximum);

        const auto after = playWith (values);
        auto difference = 0.0f;

        for (size_t i = 0; i < after.left.size(); ++i)
            difference = std::max ({ difference, std::abs (after.left[i] - before.left[i]), std::abs (after.right[i] - before.right[i]) });

        CHECK (difference > 1.0e-4f);
    }
}