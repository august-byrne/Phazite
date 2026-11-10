// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's sampler (src/devices/sampler/): what it finds in a sound (its pitch, where its hits begin, its zero crossings),
// its factory sounds, and the sampler itself: a note at its root plays the sound as it is and an octave up twice as fast,
// read pitched up from the sound's octaves, a loop holds a note on without a click at its seam, One-Shot plays to the end whatever the note and Gate lets go, slices
// by transients, beats, equal parts and by hand (added and taken away) each on its note, choke, play-through, reverse,
// mono legato that glides without starting the sound again, sounds swapped while playing, and that it plays the same
// every time.
#include "TestHelpers.h"
#include "devices/sampler/FactorySamples.h"
#include "devices/sampler/SampleAnalysis.h"
#include "devices/sampler/Sampler.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
namespace devices = phazite::devices;
using devices::NoteEvent;
using devices::Sampler;
constexpr double testRate = 48000.0;
constexpr int testBlockSize = 256;
constexpr double twoPi = juce::MathConstants<double>::twoPi;

std::vector<float> getDefaults()
{
    std::vector<float> values;

    for (const auto& parameter : Sampler::info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

// The sound as it is: no velocity, the level at 0 dB, the root at middle C.
std::vector<float> getPlainValues()
{
    auto values = getDefaults();
    values[Sampler::velocity] = 0.0f;
    values[Sampler::level] = 0.0f;
    values[Sampler::root] = 60.0f;
    return values;
}

NoteEvent noteOn (int sample, int note, float velocity = 1.0f)
{
    return { NoteEvent::Type::noteOn, sample, note, velocity };
}

NoteEvent noteOff (int sample, int note)
{
    return { NoteEvent::Type::noteOff, sample, note, 0.0f };
}

// A sound of a function of its sample, in mono, at the test's rate.
std::shared_ptr<Sampler::Sound> makeSound (int length, const std::function<float (int)>& at)
{
    auto sound = std::make_shared<Sampler::Sound>();
    sound->audio.setSize (1, length);
    sound->sampleRate = testRate;

    for (int i = 0; i < length; ++i)
        sound->audio.setSample (0, i, at (i));

    sound->onsets = devices::detectOnsets (sound->audio, sound->sampleRate);
    Sampler::makeOctaves (*sound);
    return sound;
}

std::shared_ptr<Sampler::Sound> makeSine (double frequency, double seconds, float level = 0.5f)
{
    return makeSound ((int) (seconds * testRate), [=] (int i) { return level * (float) std::sin (twoPi * frequency * i / testRate); });
}

// Clicks: a short decaying burst every spacing samples, the first at the start.
std::shared_ptr<Sampler::Sound> makeClicks (int count, int spacing)
{
    return makeSound (count * spacing,
                      [=] (int i)
                      {
                          const auto t = i % spacing;
                          return t < 2000 ? 0.8f * (float) std::sin (twoPi * 800.0 * t / testRate) * std::exp (-(float) t / 300.0f) : 0.0f;
                      });
}

std::unique_ptr<Sampler> makeSampler (std::shared_ptr<const Sampler::Sound> sound)
{
    auto sampler = std::make_unique<Sampler>();
    sampler->prepare (testRate, testBlockSize);
    sampler->setSound (std::move (sound));
    return sampler;
}

std::vector<float> play (Sampler& sampler, const std::vector<float>& values, int numSamples, const std::vector<NoteEvent>& events,
                         double tempo = 120.0)
{
    juce::AudioBuffer<float> buffer (2, testBlockSize);
    std::vector<float> played;
    std::vector<NoteEvent> inBlock;

    for (int done = 0; done < numSamples; done += testBlockSize)
    {
        const auto length = std::min (testBlockSize, numSamples - done);
        buffer.clear();
        inBlock.clear();

        for (auto event : events)
        {
            if (event.sample >= done && event.sample < done + length)
            {
                event.sample -= done;
                inBlock.push_back (event);
            }
        }

        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = length;
        block.parameters = values;
        block.events = inBlock;
        block.tempo = tempo;
        block.isPlaying = true;
        sampler.process (block);

        for (int i = 0; i < length; ++i)
            played.push_back (buffer.getSample (0, i));
    }

    return played;
}

double measureFrequency (const std::vector<float>& sound, size_t from, size_t to)
{
    size_t first = 0, last = 0;
    auto crossings = 0;

    for (auto i = from + 1; i < to; ++i)
    {
        if (sound[i - 1] < 0.0f && sound[i] >= 0.0f)
        {
            if (crossings == 0)
                first = i;

            last = i;
            ++crossings;
        }
    }

    return crossings > 1 ? (crossings - 1) * testRate / (double) (last - first) : 0.0;
}

float peakOf (const std::vector<float>& sound, size_t from, size_t to)
{
    auto peak = 0.0f;

    for (auto i = from; i < std::min (to, sound.size()); ++i)
        peak = std::max (peak, std::abs (sound[i]));

    return peak;
}

// The largest step from one sample to the next, after a start.
float largestStep (const std::vector<float>& sound, size_t from, size_t to)
{
    auto largest = 0.0f;

    for (auto i = std::max<size_t> (from, 1); i < std::min (to, sound.size()); ++i)
        largest = std::max (largest, std::abs (sound[i] - sound[i - 1]));

    return largest;
}
} // namespace

TEST_CASE ("The sampler's description holds together")
{
    REQUIRE (devices::isValid (Sampler::info));
    CHECK (Sampler::info.kind == devices::DeviceKind::instrument);
    CHECK (Sampler::info.name == std::string_view ("Sampler"));
    CHECK (Sampler::info.parameters.size() == (size_t) Sampler::level + 1);
    CHECK (devices::findParameter (Sampler::info, "root") == Sampler::root);
    CHECK (devices::findParameter (Sampler::info, "sliceBy") == Sampler::sliceBy);
    CHECK (devices::findParameter (Sampler::info, "level") == Sampler::level);
    CHECK (Sampler::info.parameters[Sampler::root].unit == devices::Unit::note);
}

TEST_CASE ("The sampler finds a sound's pitch past its attack, and none in noise")
{
    // A sine at A2 (110 Hz, note 45), and one at C2 after a sweep down from an octave above, as an 808's pitch drops.
    const auto a2 = makeSine (110.0, 1.0);
    const auto pitch = devices::detectPitch (a2->audio, testRate);
    REQUIRE (pitch.has_value());
    CHECK (*pitch == doctest::Approx (45.0).epsilon (0.002));

    auto phase = 0.0;
    const auto swept = makeSound ((int) testRate,
                                  [&phase] (int i)
                                  {
                                      const auto t = i / testRate;
                                      phase += twoPi * (65.406 + 65.406 * std::exp (-t / 0.03)) / testRate;
                                      return 0.6f * (float) std::sin (phase);
                                  });
    const auto low = devices::detectPitch (swept->audio, testRate);
    REQUIRE (low.has_value());
    CHECK (*low == doctest::Approx (36.0).epsilon (0.004));

    // The 808 that comes with the sampler is at C2, its root.
    const auto bass = devices::getFactorySample (0);
    const auto bassPitch = devices::detectPitch (bass->audio, bass->sampleRate);
    REQUIRE (bassPitch.has_value());
    CHECK (std::abs (*bassPitch - 36.0) < 0.3);
    CHECK (devices::getFactorySampleRoot (0) == 36);

    juce::Random random (3);
    const auto noise = makeSound ((int) testRate, [&random] (int) { return random.nextFloat() - 0.5f; });
    CHECK_FALSE (devices::detectPitch (noise->audio, testRate).has_value());
}

TEST_CASE ("The sampler finds where a sound's hits begin, the stronger slicing first")
{
    constexpr int spacing = 12000; // a quarter of a second
    const auto clicks = makeClicks (8, spacing);
    const auto& onsets = clicks->onsets;
    REQUIRE (onsets.size() == 8);

    for (size_t i = 0; i < onsets.size(); ++i)
    {
        CAPTURE (i);
        CHECK (onsets[i].sample == (int) i * spacing); // at its first sample, where it crosses zero
        CHECK (onsets[i].slicesFrom >= 0.0f);
        CHECK (onsets[i].slicesFrom <= 1.0f);
    }

    CHECK (onsets[0].slicesFrom == 0.0f); // the first sound always slices

    // Over a noise floor, at the zero crossing just before the hit, never in its attack.
    juce::Random random (7);
    const auto noisy =
        makeSound (4 * spacing, [&random, clicks] (int i) { return clicks->audio.getSample (0, i) + 0.01f * (random.nextFloat() - 0.5f); });
    REQUIRE (noisy->onsets.size() == 4);

    for (size_t i = 1; i < noisy->onsets.size(); ++i)
    {
        CAPTURE (i);
        CHECK (noisy->onsets[i].sample <= (int) i * spacing + 1);
        CHECK (noisy->onsets[i].sample >= (int) i * spacing - 48);
    }

    // A zero crossing is found near a place in a sine, and a place is kept where there is none.
    const auto sine = makeSine (100.0, 0.1);
    const auto crossing = devices::findZeroCrossing (sine->audio, 230, 200);
    CHECK (std::abs (sine->audio.getSample (0, crossing)) < 0.01f);
}

TEST_CASE ("A note at the root plays the sound as it is, an octave up twice as fast, against its own rate")
{
    const auto sine = makeSine (220.0, 1.0);
    auto sampler = makeSampler (sine);
    auto values = getPlainValues();
    values[Sampler::attack] = 0.5f;

    const auto atRoot = play (*sampler, values, 24000, { noteOn (0, 60) });
    CHECK (measureFrequency (atRoot, 2000, 20000) == doctest::Approx (220.0).epsilon (0.002));
    CHECK (peakOf (atRoot, 2000, 20000) == doctest::Approx (0.5f).epsilon (0.02));

    sampler->reset();
    const auto octaveUp = play (*sampler, values, 24000, { noteOn (0, 72) });
    CHECK (measureFrequency (octaveUp, 2000, 20000) == doctest::Approx (440.0).epsilon (0.003));

    // Transpose and fine move it too; the root moves where it plays at its own pitch.
    sampler->reset();
    values[Sampler::transpose] = -12.0f;
    CHECK (measureFrequency (play (*sampler, values, 24000, { noteOn (0, 60) }), 2000, 20000) == doctest::Approx (110.0).epsilon (0.003));

    sampler->reset();
    values[Sampler::transpose] = 0.0f;
    values[Sampler::root] = 48.0f;
    CHECK (measureFrequency (play (*sampler, values, 24000, { noteOn (0, 48) }), 2000, 20000) == doctest::Approx (220.0).epsilon (0.002));

    // A sound at another rate plays at its pitch.
    auto other = std::make_shared<Sampler::Sound> (*sine);
    other->sampleRate = testRate / 2.0; // as if recorded at 24 kHz: its 220 Hz is 110 Hz
    sampler->setSound (other);
    sampler->reset();
    CHECK (measureFrequency (play (*sampler, values, 24000, { noteOn (0, 48) }), 2000, 20000) == doctest::Approx (110.0).epsilon (0.003));
}

TEST_CASE ("Pitched up octaves, the sampler reads its sound's octaves: in tune, as loud, nothing folded back, as the sound reads")
{
    auto values = getPlainValues();
    values[Sampler::attack] = 0.5f;

    // A 3 kHz sine two octaves up is a 12 kHz one, as loud.
    auto sampler = makeSampler (makeSine (3000.0, 1.0));
    const auto up = play (*sampler, values, 24000, { noteOn (0, 84) });
    CHECK (measureFrequency (up, 2000, 20000) == doctest::Approx (12000.0).epsilon (0.002));
    CHECK (peakOf (up, 2000, 20000) == doctest::Approx (0.5f).epsilon (0.03));

    // A 10 kHz one would be at 40 kHz, past half the rate: nothing of it folds back down.
    sampler = makeSampler (makeSine (10000.0, 1.0));
    CHECK (peakOf (play (*sampler, values, 24000, { noteOn (0, 84) }), 2000, 20000) < 0.005f); // 40 dB under the sine

    // Read from its octave or from the sound itself, a note pitched up plays the same.
    const auto sine = makeSine (440.0, 1.0);
    auto plain = std::make_shared<Sampler::Sound> (*sine);

    for (auto& octave : plain->octaves)
        octave.setSize (0, 0);

    auto fromOctave = makeSampler (sine);
    auto fromSound = makeSampler (plain);
    const auto a = play (*fromOctave, values, 24000, { noteOn (0, 79) }), b = play (*fromSound, values, 24000, { noteOn (0, 79) });
    auto largest = 0.0f;

    for (size_t i = 0; i < a.size(); ++i)
        largest = std::max (largest, std::abs (a[i] - b[i]));

    CHECK (largest < 0.005f);
}

TEST_CASE ("The sampler's loop holds a note on past its sound's end, its seam crossfaded so that it doesn't click")
{
    // A sine of a frequency whose cycles don't fit the loop, so that an uncrossfaded seam would jump.
    const auto sine = makeSine (233.0, 0.5);
    auto sampler = makeSampler (sine);
    auto values = getPlainValues();
    values[Sampler::loopOn] = 1.0f;
    values[Sampler::loopStart] = 0.4f;
    values[Sampler::loopEnd] = 0.9f;
    values[Sampler::loopCrossfade] = 0.2f;

    const auto held = play (*sampler, values, (int) (2.0 * testRate), { noteOn (0, 60) });
    CHECK (peakOf (held, (size_t) (1.5 * testRate), (size_t) (2.0 * testRate)) > 0.3f); // still playing after 2 s

    // No step bigger than the sine's own (2 pi f / rate of its level), with some room for the crossfade's dip.
    const auto ownStep = 0.5f * (float) (twoPi * 233.0 / testRate);
    CHECK (largestStep (held, 1000, held.size()) < ownStep * 1.6f);

    // Without the loop, it stops where the sound ends.
    values[Sampler::loopOn] = 0.0f;
    sampler->reset();
    const auto once = play (*sampler, values, (int) testRate, { noteOn (0, 60) });
    CHECK (peakOf (once, (size_t) (0.51 * testRate), once.size()) == 0.0f);
}

TEST_CASE ("A loop that starts where the sound starts plays round without a dip: the start's fade in is for its first time only")
{
    // A steady level, played from a quarter in, its loop from there (Loop Start before Start is Start) to three quarters,
    // without a crossfade: each time round began at the start's few samples' fade in again, a dip to silence, a click.
    auto steady = makeSound (24000, [] (int) { return 0.5f; });
    auto sampler = makeSampler (steady);
    auto values = getPlainValues();
    values[Sampler::start] = 0.25f;
    values[Sampler::loopOn] = 1.0f;
    values[Sampler::loopStart] = 0.0f;
    values[Sampler::loopEnd] = 0.75f;
    values[Sampler::loopCrossfade] = 0.0f;

    const auto held = play (*sampler, values, (int) testRate, { noteOn (0, 60) });
    auto lowest = 1.0f;

    for (auto i = (size_t) (0.1 * testRate); i < held.size(); ++i)
        lowest = std::min (lowest, std::abs (held[i]));

    CHECK (lowest > 0.4f);
}

TEST_CASE ("A note on a sampler voice that Stop was fading plays at its full level, though the voice's release ended first")
{
    // Mono, released in a millisecond: Stop just after its note began fades the voice over 10 ms, and its release ends first,
    // which left the voice fading for the next note, which then died in a few milliseconds.
    auto values = getPlainValues();
    values[Sampler::voiceMode] = 1.0f; // mono: every note on the one voice
    values[Sampler::release] = 1.0f;
    juce::AudioBuffer<float> buffer (2, testBlockSize);
    std::vector<NoteEvent> events;
    const auto playBlock = [&buffer, &events, &values] (Sampler& sampler, bool songPlays, std::vector<NoteEvent> inBlock)
    {
        buffer.clear();
        events = std::move (inBlock);
        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = testBlockSize;
        block.parameters = values;
        block.events = events;
        block.tempo = 120.0;
        block.isPlaying = songPlays;
        sampler.process (block);
        return buffer.getMagnitude (0, 0, testBlockSize);
    };

    const auto loudestOfNote = [&playBlock] (Sampler& sampler)
    {
        playBlock (sampler, true, { noteOn (0, 60) });
        auto loudest = 0.0f;

        for (int i = 0; i < 8; ++i)
            loudest = std::max (loudest, playBlock (sampler, true, {}));

        return loudest;
    };

    const auto sine = makeSine (440.0, 1.0);
    const auto full = loudestOfNote (*makeSampler (sine));
    REQUIRE (full > 0.1f);

    auto sampler = makeSampler (sine);
    playBlock (*sampler, true, { noteOn (0, 60) });
    playBlock (*sampler, false, { { NoteEvent::Type::allNotesOff, 0, 0, 0.0f } });

    for (int i = 0; i < 8; ++i)
        playBlock (*sampler, false, {});

    CHECK (loudestOfNote (*sampler) == doctest::Approx (full).epsilon (0.01));
}

TEST_CASE ("One-Shot plays to the end whatever the note, and with Gate lets go over its fade out")
{
    const auto sine = makeSine (300.0, 0.5);
    auto sampler = makeSampler (sine);
    auto values = getPlainValues();
    values[Sampler::mode] = (float) Sampler::Mode::oneShot;
    values[Sampler::fadeOut] = 10.0f;

    // A note a tenth of a second long plays all half second.
    const auto shortNote = play (*sampler, values, (int) testRate, { noteOn (0, 60), noteOff (4800, 60) });
    CHECK (peakOf (shortNote, 20000, 22000) > 0.4f);
    CHECK (peakOf (shortNote, 24500, shortNote.size()) < 0.001f);

    // Gated, it lets go as the note does.
    values[Sampler::playback] = 1.0f;
    sampler->reset();
    const auto gated = play (*sampler, values, (int) testRate, { noteOn (0, 60), noteOff (4800, 60) });
    CHECK (peakOf (gated, 2000, 4000) > 0.4f);
    CHECK (peakOf (gated, 6000, gated.size()) < 0.001f);

    // Its fade in shapes its start.
    values[Sampler::playback] = 0.0f;
    values[Sampler::fadeIn] = 100.0f;
    sampler->reset();
    const auto faded = play (*sampler, values, 12000, { noteOn (0, 60) });
    CHECK (peakOf (faded, 0, 480) < 0.1f);
    CHECK (peakOf (faded, 6000, 8000) > 0.4f);
}

TEST_CASE ("Slice mode puts each slice on its note from 36 up, by transients, beats, equal parts or by hand")
{
    constexpr int spacing = 12000;
    const auto clicks = makeClicks (8, spacing);
    auto values = getPlainValues();
    values[Sampler::mode] = (float) Sampler::Mode::slice;

    // Every transient at full sensitivity: eight slices, each starting at its hit.
    values[Sampler::sensitivity] = 1.0f;
    auto slices = Sampler::getSlices (*clicks, nullptr, values, 120.0);
    REQUIRE (slices.count == 8);
    CHECK (slices.bounds[0] == 0);
    CHECK (slices.bounds[8] == clicks->audio.getNumSamples());

    // At none, the sound whole.
    values[Sampler::sensitivity] = 0.0f;
    CHECK (Sampler::getSlices (*clicks, nullptr, values, 120.0).count == 1);

    // Beats: a quarter second is an eighth note at 120 BPM; the sound's own beats win where it says them.
    values[Sampler::sliceBy] = (float) Sampler::SliceBy::beats;
    values[Sampler::sliceDivision] = 5.0f; // an eighth
    CHECK (Sampler::getSlices (*clicks, nullptr, values, 120.0).count == 8);
    auto known = std::make_shared<Sampler::Sound> (*clicks);
    known->beats = 8.0; // eight beats: an eighth note is half its quarter second
    CHECK (Sampler::getSlices (*known, nullptr, values, 120.0).count == 16);

    // Equal parts.
    values[Sampler::sliceBy] = (float) Sampler::SliceBy::equal;
    values[Sampler::sliceCount] = 4.0f;
    slices = Sampler::getSlices (*clicks, nullptr, values, 120.0);
    REQUIRE (slices.count == 4);
    CHECK (slices.bounds[1] == 2 * spacing);

    // By hand: one added stays with any method, one taken away stays away.
    Sampler::ManualSlices manual;
    manual.added = { 0.1f };
    manual.removed = { 0.5f };
    slices = Sampler::getSlices (*clicks, &manual, values, 120.0);
    REQUIRE (slices.count == 4); // 0, 0.1 (by hand), 0.25, 0.75: 0.5 taken away
    CHECK (slices.byHand[1]);
    CHECK (slices.bounds[1] == (int) std::lround (0.1 * clicks->audio.getNumSamples()));
    CHECK (slices.bounds[3] == 6 * spacing);

    values[Sampler::sliceBy] = (float) Sampler::SliceBy::manual;
    CHECK (Sampler::getSlices (*clicks, &manual, values, 120.0).count == 2); // the start and the one by hand

    // Each slice plays on its note: note 38 the third.
    values[Sampler::sliceBy] = (float) Sampler::SliceBy::transients;
    values[Sampler::sensitivity] = 1.0f;
    auto sampler = makeSampler (clicks);
    const auto third = play (*sampler, values, 6000, { noteOn (0, Sampler::firstSliceNote + 2) });
    const auto expected = clicks->audio.getSample (0, 2 * spacing + 300 + 64);
    CHECK (third[300 + 64] == doctest::Approx (expected).epsilon (0.05)); // the third hit, after the start's fade
    CHECK (play (*sampler, values, 2000, { noteOn (0, Sampler::firstSliceNote + 20) }) == std::vector<float> (2000, 0.0f)); // none there
}

TEST_CASE ("A slice cuts off the one playing with choke, plays to its end unless Play Through, and reverses")
{
    constexpr int spacing = 12000;
    const auto clicks = makeClicks (4, spacing);
    auto values = getPlainValues();
    values[Sampler::mode] = (float) Sampler::Mode::slice;
    values[Sampler::sensitivity] = 1.0f;
    auto sampler = makeSampler (clicks);

    // Two at once: with choke, one voice; without, two.
    play (*sampler, values, 512, { noteOn (0, 36), noteOn (256, 37) });
    CHECK (sampler->getActiveVoices() == 1);
    values[Sampler::choke] = 0.0f;
    sampler->reset();
    play (*sampler, values, 512, { noteOn (0, 36), noteOn (256, 37) });
    CHECK (sampler->getActiveVoices() == 2);

    // A slice ends where the next begins, or plays on through the sound.
    sampler->reset();
    const auto first = play (*sampler, values, 3 * spacing, { noteOn (0, 36) });
    CHECK (peakOf (first, spacing + 100, 2 * spacing) == 0.0f);
    values[Sampler::playThrough] = 1.0f;
    sampler->reset();
    const auto through = play (*sampler, values, 3 * spacing, { noteOn (0, 36) });
    CHECK (peakOf (through, spacing + 100, spacing + 2000) > 0.3f);

    // Reversed, a ramp plays falling.
    const auto ramp = makeSound (4800, [] (int i) { return (float) i / 4800.0f; });
    auto rampValues = getPlainValues();
    rampValues[Sampler::reverse] = 1.0f;
    auto reversed = makeSampler (ramp);
    const auto played = play (*reversed, rampValues, 4800, { noteOn (0, 60) });
    CHECK (played[1000] > played[3000]);
}

TEST_CASE ("The sampler's mono mode glides between held keys without starting the sound again, as an 808 line does")
{
    const auto sine = makeSine (110.0, 2.0);
    auto sampler = makeSampler (sine);
    auto values = getPlainValues();
    values[Sampler::voiceMode] = 1.0f;
    values[Sampler::glide] = 100.0f;

    // A key, then a fifth above it while it is held: one voice, which glides there.
    const auto played = play (*sampler, values, (int) testRate, { noteOn (0, 60), noteOn (12000, 67) });
    CHECK (sampler->getActiveVoices() == 1);
    CHECK (measureFrequency (played, 2000, 11000) == doctest::Approx (110.0).epsilon (0.003));
    CHECK (measureFrequency (played, 36000, 47000) == doctest::Approx (110.0 * std::pow (2.0, 7.0 / 12.0)).epsilon (0.004));

    // No jump where the second key came: the sound played on.
    CHECK (largestStep (played, 11900, 12200) < 0.05f);
}

TEST_CASE ("The sampler swaps its sound while it plays without a click past a short fade, and plays the same every time")
{
    const auto first = makeSine (220.0, 1.0);
    auto sampler = makeSampler (first);
    const auto values = getPlainValues();
    play (*sampler, values, 4096, { noteOn (0, 60) });
    CHECK (sampler->getActiveVoices() == 1);

    sampler->setSound (makeSine (330.0, 1.0));
    play (*sampler, values, 512, {});
    CHECK (sampler->getActiveVoices() == 0); // the old sound's voice faded and went

    // Twice from the same start, the same.
    auto again = makeSampler (first);
    auto other = makeSampler (first);
    CHECK (play (*again, values, 9000, { noteOn (0, 64), noteOn (100, 67) })
           == play (*other, values, 9000, { noteOn (0, 64), noteOn (100, 67) }));

    // A new sampler plays the 808 that comes with it.
    auto fresh = std::make_unique<Sampler>();
    fresh->prepare (testRate, testBlockSize);
    REQUIRE (fresh->getSound() != nullptr);
    CHECK (fresh->getSound() == devices::getFactorySample (0).get());
    CHECK (peakOf (play (*fresh, getDefaults(), 9600, { noteOn (0, 36) }), 0, 9600) > 0.05f);
}
