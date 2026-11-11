// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's sampler (src/devices/sampler/): what it finds in a sound (its pitch, where its hits begin, its zero crossings),
// its factory sounds, and the sampler itself: a note at its root plays the sound as it is and an octave up twice as fast,
// read pitched up from the sound's octaves, a loop holds a note on without a click at its seam, One-Shot plays to the end whatever the note and Gate lets go, slices
// by transients, beats, equal parts and by hand (added and taken away) each on its note, choke, play-through, reverse,
// mono legato that glides without starting the sound again, a key played again (as it rings out, while it is held or under
// the pedal, a one-shot and a slice too), each note on a voice of its own, sounds swapped while playing, and that it plays
// the same every time; Key Time, Filter Velocity and Start Random; a seamless sound looped whole without a seam; and Wide
// Saw, the factory sound made from its spectrum.
#include "TestHelpers.h"
#include "devices/dsp/NoteValues.h"
#include "devices/sampler/FactorySamples.h"
#include "devices/sampler/SampleAnalysis.h"
#include "devices/sampler/Sampler.h"

#include <juce_dsp/juce_dsp.h>

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

// What it plays, on the left and on the right; and, if asked, how many voices sound after each block.
std::pair<std::vector<float>, std::vector<float>> playBoth (Sampler& sampler, const std::vector<float>& values, int numSamples,
                                                            const std::vector<NoteEvent>& events, double tempo = 120.0,
                                                            std::vector<int>* voicesAfter = nullptr)
{
    juce::AudioBuffer<float> buffer (2, testBlockSize);
    std::vector<float> played, playedRight;
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

        if (voicesAfter != nullptr)
            voicesAfter->push_back (sampler.getActiveVoices());

        for (int i = 0; i < length; ++i)
        {
            played.push_back (buffer.getSample (0, i));
            playedRight.push_back (buffer.getSample (1, i));
        }
    }

    return { std::move (played), std::move (playedRight) };
}

// What it plays on the left.
std::vector<float> play (Sampler& sampler, const std::vector<float>& values, int numSamples, const std::vector<NoteEvent>& events,
                         double tempo = 120.0)
{
    return playBoth (sampler, values, numSamples, events, tempo).first;
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

using Sides = std::pair<std::vector<float>, std::vector<float>>; // what it plays on the left and on the right

// The largest difference between two sounds, on either side, from a sample on.
float largestDifference (const Sides& a, const Sides& b, size_t from = 0)
{
    auto largest = 0.0f;

    for (auto i = from; i < a.first.size() && i < b.first.size(); ++i)
        largest = std::max ({ largest, std::abs (a.first[i] - b.first[i]), std::abs (a.second[i] - b.second[i]) });

    return largest;
}

NoteEvent pedal (int sample, bool down)
{
    return { NoteEvent::Type::controller, sample, 64, down ? 1.0f : 0.0f };
}

// A tone at 220 Hz with its second and third harmonics, steady for its length.
std::shared_ptr<Sampler::Sound> makeTone (double seconds)
{
    return makeSound ((int) (seconds * testRate),
                      [] (int i)
                      {
                          const auto t = twoPi * 220.0 * i / testRate;
                          return (float) (0.4 * std::sin (t) + 0.2 * std::sin (2.0 * t) + 0.1 * std::sin (3.0 * t));
                      });
}

// What a new sampler of a sound plays of some events, with the voices sounding after each block, and what new samplers of
// it play of each group of them alone, added: a key played again plays each of its notes as it would alone, what rings out
// of the one before untouched and the new one from its start, as a note on a voice of its own does. The tests' events are
// on the sampler's control grid (16 samples), as its blocks are, so that each note's voice moves at the same samples both
// ways.
struct Apart
{
    Sides together, added;
    std::vector<int> voicesAfter;
};

Apart playApart (const std::shared_ptr<Sampler::Sound>& sound, const std::vector<float>& values, int numSamples,
                 const std::vector<NoteEvent>& together, const std::vector<std::vector<NoteEvent>>& groups)
{
    Apart played;
    played.together = playBoth (*makeSampler (sound), values, numSamples, together, 120.0, &played.voicesAfter);
    played.added = { std::vector<float> (played.together.first.size()), std::vector<float> (played.together.second.size()) };

    for (const auto& group : groups)
    {
        const auto part = playBoth (*makeSampler (sound), values, numSamples, group);

        for (size_t i = 0; i < part.first.size(); ++i)
        {
            played.added.first[i] += part.first[i];
            played.added.second[i] += part.second[i];
        }
    }

    return played;
}

// A sampler's Classic mode on the tone, through its filter, which its envelope opens for each note, released over 200 ms.
std::vector<float> getFilteredValues()
{
    auto values = getPlainValues();
    values[Sampler::release] = 200.0f;
    values[Sampler::filterOn] = 1.0f;
    values[Sampler::filterEnvelopeOn] = 1.0f;
    values[Sampler::cutoff] = 1500.0f;
    return values;
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

    // Start Random beside Start, Key Time after the amp envelope and Filter Velocity after Key Tracking, each at 0 until
    // turned up: a sampler without them plays as it did.
    CHECK (devices::findParameter (Sampler::info, "startRandom") == Sampler::startRandom);
    CHECK (devices::findParameter (Sampler::info, "keyTime") == Sampler::keyTime);
    CHECK (devices::findParameter (Sampler::info, "filterVelocity") == Sampler::filterVelocity);
    CHECK (Sampler::startRandom == Sampler::start + 1);
    CHECK (Sampler::keyTime == Sampler::releaseCurve + 1);
    CHECK (Sampler::filterVelocity == Sampler::keyTracking + 1);

    for (const auto parameter : { Sampler::startRandom, Sampler::keyTime, Sampler::filterVelocity })
    {
        const auto& described = Sampler::info.parameters[(size_t) parameter];
        CHECK (described.defaultValue == 0.0f);
        CHECK (described.minimum == 0.0f);
        CHECK (described.maximum == 1.0f);
        CHECK (described.unit == devices::Unit::percent);
    }
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
    auto slices = Sampler::getSlices (*clicks, nullptr, values, 120.0, 4.0);
    REQUIRE (slices.count == 8);
    CHECK (slices.bounds[0] == 0);
    CHECK (slices.bounds[8] == clicks->audio.getNumSamples());

    // At none, the sound whole.
    values[Sampler::sensitivity] = 0.0f;
    CHECK (Sampler::getSlices (*clicks, nullptr, values, 120.0, 4.0).count == 1);

    // Beats: a quarter second is an eighth note at 120 BPM; the sound's own beats win where it says them.
    values[Sampler::sliceBy] = (float) Sampler::SliceBy::beats;
    values[Sampler::sliceDivision] = 5.0f; // an eighth
    CHECK (Sampler::getSlices (*clicks, nullptr, values, 120.0, 4.0).count == 8);
    auto known = std::make_shared<Sampler::Sound> (*clicks);
    known->beats = 8.0; // eight beats: an eighth note is half its quarter second
    CHECK (Sampler::getSlices (*known, nullptr, values, 120.0, 4.0).count == 16);

    // A bar is the song's: four beats in 4/4, the two seconds whole; three in 6/8, a slice a second and a half in.
    values[Sampler::sliceDivision] = phazite::devices::dsp::barNoteValue;
    CHECK (Sampler::getSlices (*clicks, nullptr, values, 120.0, 4.0).count == 1);
    slices = Sampler::getSlices (*clicks, nullptr, values, 120.0, 3.0);
    REQUIRE (slices.count == 2);
    CHECK (slices.bounds[1] == 6 * spacing);
    values[Sampler::sliceDivision] = 5.0f;

    // Equal parts.
    values[Sampler::sliceBy] = (float) Sampler::SliceBy::equal;
    values[Sampler::sliceCount] = 4.0f;
    slices = Sampler::getSlices (*clicks, nullptr, values, 120.0, 4.0);
    REQUIRE (slices.count == 4);
    CHECK (slices.bounds[1] == 2 * spacing);

    // By hand: one added stays with any method, one taken away stays away.
    Sampler::ManualSlices manual;
    manual.added = { 0.1f };
    manual.removed = { 0.5f };
    slices = Sampler::getSlices (*clicks, &manual, values, 120.0, 4.0);
    REQUIRE (slices.count == 4); // 0, 0.1 (by hand), 0.25, 0.75: 0.5 taken away
    CHECK (slices.byHand[1]);
    CHECK (slices.bounds[1] == (int) std::lround (0.1 * clicks->audio.getNumSamples()));
    CHECK (slices.bounds[3] == 6 * spacing);

    values[Sampler::sliceBy] = (float) Sampler::SliceBy::manual;
    CHECK (Sampler::getSlices (*clicks, &manual, values, 120.0, 4.0).count == 2); // the start and the one by hand

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

TEST_CASE ("A key played again on the sampler as it rings out takes a voice of its own: its tail rings on, the new note starts afresh")
{
    // A tone through the filter, which its envelope opens for each note, two semitones over its root: let go, then played
    // again a third of the way through its 200 ms release. It plays each note as it does alone, added, on both sides: the
    // first's tail as it would have rung out, the second from the sound's start, as a note on a voice of its own. A voice
    // taken back for the second would have faded the tail out within a few milliseconds.
    const auto tone = makeTone (1.5);
    const auto played =
        playApart (tone, getFilteredValues(), 36000, { noteOn (0, 62), noteOff (4800, 62), noteOn (8000, 62), noteOff (19200, 62) },
                   { { noteOn (0, 62), noteOff (4800, 62) }, { noteOn (8000, 62), noteOff (19200, 62) } });
    REQUIRE (peakOf (played.together.first, 0, 4800) > 0.1f);
    CHECK (largestDifference (played.together, played.added) < 1.0e-6f);
    CHECK (played.voicesAfter[40] == 2); // 0.22 s: the first's tail with the second
    CHECK (played.voicesAfter[60] == 1); // 0.33 s: the tail has rung out, at 0.3 s
    CHECK (played.voicesAfter.back() == 0);

    // A one-shot hit again plays on to its end under the next, as a drum sampler's pad does.
    auto oneShot = getPlainValues();
    oneShot[Sampler::mode] = (float) Sampler::Mode::oneShot;
    const auto hits = playApart (makeTone (0.25), oneShot, 24000, { noteOn (0, 60), noteOff (1600, 60), noteOn (4800, 60) },
                                 { { noteOn (0, 60), noteOff (1600, 60) }, { noteOn (4800, 60) } });
    CHECK (largestDifference (hits.together, hits.added) < 1.0e-6f);
    CHECK (hits.voicesAfter[25] == 2); // 0.14 s: the first plays on to its end at 0.25 s, under the second

    // So does a slice while choke is off; with choke, a slice cuts off the one playing, its own as any other.
    auto sliced = getPlainValues();
    sliced[Sampler::mode] = (float) Sampler::Mode::slice;
    sliced[Sampler::sensitivity] = 1.0f;
    sliced[Sampler::choke] = 0.0f;
    const auto clicks = makeClicks (4, 12000);
    const auto slices =
        playApart (clicks, sliced, 24000, { noteOn (0, 36), noteOn (480, 36) }, { { noteOn (0, 36) }, { noteOn (480, 36) } });
    REQUIRE (peakOf (slices.together.first, 400, 480) > 0.1f); // the first hit still rings as the second comes
    CHECK (largestDifference (slices.together, slices.added) < 1.0e-6f);
    CHECK (slices.voicesAfter[3] == 2);
    sliced[Sampler::choke] = 1.0f;
    CHECK (playApart (clicks, sliced, 4096, { noteOn (0, 36), noteOn (480, 36) }, {}).voicesAfter[3] == 1);
}

TEST_CASE ("A note-off lets go of its key's held voice only on the sampler: a tail of the same key rings on, its release not begun again")
{
    // The tone let go, a note-off while only its tail sounds, which lets go of nothing, then the same key again, let go while
    // the first's tail still rings: that note-off lets go of the second note only. Begun again, the first's release would
    // have rung on past the 200 ms it takes.
    const auto played = playApart (makeTone (1.5), getFilteredValues(), 36000,
                                   { noteOn (0, 62), noteOff (4800, 62), noteOff (6400, 62), noteOn (8000, 62), noteOff (9600, 62) },
                                   { { noteOn (0, 62), noteOff (4800, 62) }, { noteOn (8000, 62), noteOff (9600, 62) } });
    CHECK (largestDifference (played.together, played.added) < 1.0e-6f);
    CHECK (played.voicesAfter[40] == 2); // the two tails
    CHECK (played.voicesAfter[60] == 1); // 0.33 s: the first's rang out at 0.3 s, the second's rings until 0.4 s
    CHECK (played.voicesAfter[76] == 0);
}

TEST_CASE ("A key played again on the sampler while it is held lets its voice go as if it came up; the new note takes a voice of its own")
{
    // Two notes of the same key that overlap: the first lets go as the second begins, its release ringing on, and the first
    // note-off lets go of the key, which then holds nothing for the second. A key never holds two voices.
    const auto played = playApart (makeTone (1.5), getFilteredValues(), 36000,
                                   { noteOn (0, 62), noteOn (3200, 62), noteOff (4800, 62), noteOff (8000, 62) },
                                   { { noteOn (0, 62), noteOff (3200, 62) }, { noteOn (3200, 62), noteOff (4800, 62) } });
    CHECK (largestDifference (played.together, played.added) < 1.0e-6f);
    CHECK (played.voicesAfter[14] == 2); // 0.08 s: the first's tail, and the second held
}

TEST_CASE ("Under the sustain pedal, a key played again on the sampler lets the voice the pedal holds ring out, and the pedal holds the "
           "new one")
{
    // The pedal down, the tone played and let go: the pedal holds it. Played again, the voice the pedal held lets go, its
    // release ringing on, and the new note takes a voice of its own, which the pedal holds until it lifts: one voice held for
    // the key.
    const auto played = playApart (
        makeTone (1.5), getFilteredValues(), 36000,
        { pedal (0, true), noteOn (0, 62), noteOff (1600, 62), noteOn (4800, 62), noteOff (6400, 62), pedal (9600, false) },
        { { noteOn (0, 62), noteOff (4800, 62) }, { pedal (0, true), noteOn (4800, 62), noteOff (6400, 62), pedal (9600, false) } });
    CHECK (largestDifference (played.together, played.added) < 1.0e-6f);
    CHECK (played.voicesAfter[30] == 2); // 0.17 s: the first's tail, and the second that the pedal holds
    CHECK (played.voicesAfter[60] == 1); // 0.33 s: the first's tail rang out at 0.3 s; the pedal let go of the second at 0.2 s
    CHECK (played.voicesAfter[76] == 0);
}

TEST_CASE ("In mono mode a key played again on the sampler as it rings out takes the one voice, its tail faded out, as before")
{
    // The tone released over 200 ms and played again a third of the way through: one voice still, whose tail fades out over a
    // few milliseconds while the sound starts again from its start, as a new sampler's first note does.
    auto values = getFilteredValues();
    values[Sampler::voiceMode] = 1.0f;
    const auto tone = makeTone (1.5);
    std::vector<int> voicesAfter;
    const auto played = playBoth (*makeSampler (tone), values, 36000,
                                  { noteOn (0, 62), noteOff (4800, 62), noteOn (8000, 62), noteOff (19200, 62) }, 120.0, &voicesAfter);
    const auto second = playBoth (*makeSampler (tone), values, 36000, { noteOn (8000, 62), noteOff (19200, 62) });
    CHECK (*std::max_element (voicesAfter.begin(), voicesAfter.end()) == 1);
    REQUIRE (peakOf (played.first, 7600, 8000) > 0.02f); // its tail, as it is played again
    CHECK (largestDifference (played, second, 8192) < 1.0e-6f);
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

TEST_CASE ("Key Time makes the amp envelope's times shorter up the keys from the root, as the FM synth's, and nothing at 0")
{
    // A steady level through straight stages: how long a key's note takes to come half-way up, down, or to let go.
    const auto steady = makeSound (4 * (int) testRate, [] (int) { return 0.5f; });
    auto values = getPlainValues(); // the root at 60
    values[Sampler::attackCurve] = values[Sampler::decayCurve] = values[Sampler::releaseCurve] = 0.0f;

    const auto milliseconds = [] (size_t samples) { return 1000.0 * (double) samples / testRate; };

    // The decay to silence: the time from the note's start until it is half-way down.
    const auto halfDecay = [&] (int note, float keyTime, float root = 60.0f)
    {
        auto sampler = makeSampler (steady);
        auto set = values;
        set[Sampler::root] = root;
        set[Sampler::keyTime] = keyTime;
        set[Sampler::attack] = 0.5f;
        set[Sampler::decay] = 200.0f;
        set[Sampler::sustain] = 0.0f;
        const auto played = play (*sampler, set, 2 * (int) testRate, { noteOn (0, note) });
        const auto found = std::find_if (played.begin() + 400, played.end(), [] (float sample) { return std::abs (sample) < 0.25f; });
        return milliseconds ((size_t) (found - played.begin()));
    };

    // At full, half the time an octave up, twice an octave down, and no shorter than an eighth or longer than eight times.
    CHECK (halfDecay (60, 1.0f) == doctest::Approx (100.0).epsilon (0.02));
    CHECK (halfDecay (72, 1.0f) == doctest::Approx (50.0).epsilon (0.02));
    CHECK (halfDecay (48, 1.0f) == doctest::Approx (200.0).epsilon (0.02));
    CHECK (halfDecay (67, 1.0f) == doctest::Approx (100.0 * std::exp2 (-7.0 / 12.0)).epsilon (0.02));
    CHECK (halfDecay (96, 1.0f) == doctest::Approx (12.5).epsilon (0.05));
    CHECK (halfDecay (108, 1.0f) == doctest::Approx (12.5).epsilon (0.05));
    CHECK (halfDecay (24, 1.0f) == doctest::Approx (800.0).epsilon (0.02));
    CHECK (halfDecay (12, 1.0f) == doctest::Approx (800.0).epsilon (0.02));

    // Part of the way, as much of that: the FM synth's law, 2^(-keyTime * octaves).
    CHECK (halfDecay (72, 0.5f) == doctest::Approx (100.0 * std::exp2 (-0.5)).epsilon (0.02));
    CHECK (halfDecay (48, 0.25f) == doctest::Approx (100.0 * std::exp2 (0.25)).epsilon (0.02));

    // From the root, wherever it is; and at 0, the times as set on every key.
    CHECK (halfDecay (48, 1.0f, 48.0f) == doctest::Approx (100.0).epsilon (0.02));
    CHECK (halfDecay (60, 1.0f, 48.0f) == doctest::Approx (50.0).epsilon (0.02));

    for (const auto note : { 36, 60, 84 })
        CHECK (halfDecay (note, 0.0f) == doctest::Approx (100.0).epsilon (0.02));

    // The attack and the release with it: every time of the envelope, as the FM synth's.
    const auto halfAttack = [&] (int note, float keyTime)
    {
        auto sampler = makeSampler (steady);
        auto set = values;
        set[Sampler::keyTime] = keyTime;
        set[Sampler::attack] = 100.0f;
        const auto played = play (*sampler, set, (int) testRate, { noteOn (0, note) });
        const auto found = std::find_if (played.begin(), played.end(), [] (float sample) { return std::abs (sample) >= 0.25f; });
        return milliseconds ((size_t) (found - played.begin()));
    };

    CHECK (halfAttack (60, 1.0f) == doctest::Approx (50.0).epsilon (0.02));
    CHECK (halfAttack (72, 1.0f) == doctest::Approx (25.0).epsilon (0.03));
    CHECK (halfAttack (48, 1.0f) == doctest::Approx (100.0).epsilon (0.02));
    CHECK (halfAttack (72, 0.0f) == doctest::Approx (50.0).epsilon (0.02));

    constexpr int letGo = 4800;
    const auto halfRelease = [&] (int note, float keyTime, Sampler::Mode mode = Sampler::Mode::classic)
    {
        auto sampler = makeSampler (steady);
        auto set = values;
        set[Sampler::mode] = (float) mode;
        set[Sampler::playback] = 1.0f; // Gate, in One-Shot
        set[Sampler::fadeOut] = 200.0f;
        set[Sampler::keyTime] = keyTime;
        set[Sampler::release] = 200.0f;
        const auto played = play (*sampler, set, (int) testRate, { noteOn (0, note), noteOff (letGo, note) });
        const auto found = std::find_if (played.begin() + letGo, played.end(), [] (float sample) { return std::abs (sample) < 0.25f; });
        return milliseconds ((size_t) (found - played.begin()) - letGo);
    };

    CHECK (halfRelease (60, 1.0f) == doctest::Approx (100.0).epsilon (0.02));
    CHECK (halfRelease (72, 1.0f) == doctest::Approx (50.0).epsilon (0.02));
    CHECK (halfRelease (72, 0.0f) == doctest::Approx (100.0).epsilon (0.02));

    // What it says of how long it rings on is its release at its longest, for what renders it to its end.
    {
        auto sampler = makeSampler (steady);
        auto set = values;
        set[Sampler::release] = 200.0f;
        play (*sampler, set, testBlockSize, {});
        CHECK (sampler->getTailSeconds() == doctest::Approx (0.2));
        set[Sampler::keyTime] = 0.5f;
        play (*sampler, set, testBlockSize, {});
        CHECK (sampler->getTailSeconds() == doctest::Approx (1.6));
    }

    // Not the fades that shape a One-Shot's note: its fade out takes its time on any key.
    CHECK (halfRelease (72, 1.0f, Sampler::Mode::oneShot)
           == doctest::Approx (halfRelease (60, 1.0f, Sampler::Mode::oneShot)).epsilon (0.02));
}

TEST_CASE ("Filter Velocity closes the sampler's filter for softer notes, as the synth's does, and not at all at 0")
{
    // A 4 kHz sine under a 12 dB low-pass at 8 kHz, without resonance or key tracking; velocity leaves the level alone.
    const auto sine = makeSine (4000.0, 1.0);
    auto values = getPlainValues();
    values[Sampler::filterOn] = 1.0f;
    values[Sampler::cutoff] = 8000.0f;
    values[Sampler::resonance] = 0.0f;
    values[Sampler::keyTracking] = 0.0f;

    struct Heard
    {
        float cutoff = 0.0f, level = 0.0f;
    };

    const auto hear = [&] (float amount, float velocity)
    {
        auto sampler = makeSampler (sine);
        auto set = values;
        set[Sampler::filterVelocity] = amount;
        const auto played = play (*sampler, set, 9600, { noteOn (0, 60, velocity) });
        return Heard { sampler->getShown().cutoff.get(), peakOf (played, 4800, 9600) };
    };

    // At full velocity, the cutoff as set, whatever the amount.
    CHECK (hear (0.5f, 1.0f).cutoff == doctest::Approx (8000.0f).epsilon (0.001));
    CHECK (hear (1.0f, 1.0f).cutoff == doctest::Approx (8000.0f).epsilon (0.001));

    // Softer, lower by the amount times four octaves times how far under full the velocity is: the synth's law.
    CHECK (hear (0.5f, 0.5f).cutoff == doctest::Approx (4000.0f).epsilon (0.001));
    CHECK (hear (1.0f, 0.5f).cutoff == doctest::Approx (2000.0f).epsilon (0.001));
    CHECK (hear (0.5f, 0.25f).cutoff == doctest::Approx (8000.0f * std::exp2 (-1.5f)).epsilon (0.001));
    CHECK (hear (0.25f, 0.8f).cutoff == doctest::Approx (8000.0f * std::exp2 (-0.2f)).epsilon (0.001));

    // And heard: the sine at the cutoff is 3 dB down, and an octave past it 12 dB or so.
    const auto open = hear (0.5f, 1.0f).level;
    CHECK (open == doctest::Approx (0.5f).epsilon (0.08));
    CHECK (hear (0.5f, 0.5f).level == doctest::Approx (open * 0.72f).epsilon (0.06));
    CHECK (hear (1.0f, 0.5f).level < open * 0.3f);

    // At 0, the same filter for every velocity.
    CHECK (hear (0.0f, 0.25f).cutoff == doctest::Approx (8000.0f).epsilon (0.001));
    CHECK (hear (0.0f, 0.25f).level == doctest::Approx (open).epsilon (0.001));
}

TEST_CASE ("Start Random starts each note somewhere else in the sound, the same every time, in its loop, without a click")
{
    const auto sine = makeSine (220.0, 1.0);
    const auto frames = (float) sine->audio.getNumSamples();
    auto values = getPlainValues();

    // Where each of some notes on one key starts, as a part of the sound: where its voice is after a block, less the block.
    // Each is let go and rings out before the next, which so takes the same voice (a key played again as it rings out
    // takes a voice of its own).
    const auto startsOf = [&] (Sampler& sampler, const std::vector<float>& set, int count, int note = 60)
    {
        std::vector<float> starts;

        for (int i = 0; i < count; ++i)
        {
            play (sampler, set, testBlockSize, { noteOn (0, note) });
            starts.push_back (sampler.getShown().positions[0].get() - (float) testBlockSize / frames);
            play (sampler, set, testBlockSize, { noteOff (0, note) });

            for (int blocks = 0; sampler.getActiveVoices() > 0 && blocks < 1000; ++blocks) // the sound is a second long
                play (sampler, set, testBlockSize, {});
        }

        return starts;
    };

    // At 0, every note from the start marker.
    auto sampler = makeSampler (sine);

    for (const auto start : startsOf (*sampler, values, 8))
        CHECK (start == doctest::Approx (0.0f).epsilon (1.0e-4));

    // At full, anywhere in the sound: no two alike, and all over it.
    values[Sampler::startRandom] = 1.0f;
    sampler = makeSampler (sine);
    const auto anywhere = startsOf (*sampler, values, 64);
    auto sorted = anywhere;
    std::sort (sorted.begin(), sorted.end());
    CHECK (std::adjacent_find (sorted.begin(), sorted.end()) == sorted.end());
    CHECK (sorted.front() >= 0.0f);
    CHECK (sorted.front() < 0.1f);
    CHECK (sorted.back() > 0.9f);
    CHECK (sorted.back() < 1.0f);
    CHECK (sorted[32] > 0.3f);
    CHECK (sorted[32] < 0.7f);

    // The same places from another sampler, and from this one once it is reset: it renders the same every time.
    auto other = makeSampler (sine);
    CHECK (startsOf (*other, values, 64) == anywhere);
    sampler->reset();
    CHECK (startsOf (*sampler, values, 64) == anywhere);

    // Part of the way, as far past the start marker as that part of what plays.
    values[Sampler::start] = 0.2f;
    values[Sampler::startRandom] = 0.25f;
    sampler = makeSampler (sine);

    for (const auto start : startsOf (*sampler, values, 32))
    {
        CHECK (start >= 0.2f - 1.0e-4f);
        CHECK (start <= 0.4f + 1.0e-4f);
    }

    // With a loop, a place past the loop's end is in the loop instead.
    values[Sampler::start] = 0.0f;
    values[Sampler::startRandom] = 1.0f;
    values[Sampler::loopOn] = 1.0f;
    values[Sampler::loopStart] = 0.5f;
    values[Sampler::loopEnd] = 0.75f;
    values[Sampler::loopCrossfade] = 0.0f;
    sampler = makeSampler (sine);
    const auto looped = startsOf (*sampler, values, 64);
    CHECK (*std::max_element (looped.begin(), looped.end()) < 0.75f);
    CHECK (*std::min_element (looped.begin(), looped.end()) < 0.5f); // before the loop too, where it plays into it
    CHECK (std::count_if (looped.begin(), looped.end(), [] (float start) { return start >= 0.5f; }) > 20);

    // One-Shot's notes too; a slice starts where it is cut.
    values[Sampler::loopOn] = 0.0f;
    values[Sampler::mode] = (float) Sampler::Mode::oneShot;
    sampler = makeSampler (sine);
    const auto shots = startsOf (*sampler, values, 16);
    CHECK (*std::max_element (shots.begin(), shots.end()) - *std::min_element (shots.begin(), shots.end()) > 0.5f);

    values[Sampler::mode] = (float) Sampler::Mode::slice;
    values[Sampler::sliceBy] = (float) Sampler::SliceBy::equal;
    values[Sampler::sliceCount] = 4.0f;
    sampler = makeSampler (sine);

    for (const auto start : startsOf (*sampler, values, 8, Sampler::firstSliceNote + 1))
        CHECK (start == doctest::Approx (0.25f).epsilon (1.0e-3));

    // No click where a note starts in the middle of a cycle: it comes in over its attack and a few samples' fade, from
    // silence, with no step much bigger than the sine's own.
    values = getPlainValues();
    values[Sampler::startRandom] = 1.0f;
    values[Sampler::attack] = 0.5f;
    sampler = makeSampler (sine);
    const auto ownStep = 0.5f * (float) (twoPi * 220.0 / testRate);

    values[Sampler::release] = 0.5f;

    for (int i = 0; i < 16; ++i)
    {
        // Each note at another place, the one before it gone.
        const auto played = play (*sampler, values, 4000, { noteOn (2000, 60), noteOff (3000, 60) });
        CHECK (std::abs (played[2000]) < 0.01f);
        CHECK (peakOf (played, 2100, 3000) > 0.45f);
        CHECK (largestStep (played, 2000, 3000) < ownStep + 0.05f);
    }
}

TEST_CASE ("A seamless sound loops over the whole of it without a seam at any pitch, read from itself or from its octaves")
{
    // A sine with a whole number of cycles in the sound: read round its end into its start, it goes on as a sine.
    constexpr int length = 1 << 14, cycles = 75;
    const auto frequency = cycles * testRate / length;
    const auto make = [&] (bool seamless)
    {
        auto sound = std::make_shared<Sampler::Sound>();
        sound->audio.setSize (2, length);
        sound->sampleRate = testRate;
        sound->seamless = seamless;

        for (int i = 0; i < length; ++i)
        {
            sound->audio.setSample (0, i, 0.5f * (float) std::cos (twoPi * cycles * i / length)); // at its full level at the seam
            sound->audio.setSample (1, i, 0.5f * (float) std::sin (twoPi * cycles * i / length));
        }

        sound->onsets = { devices::Onset {} };
        Sampler::makeOctaves (*sound);
        return sound;
    };

    auto values = getPlainValues();
    values[Sampler::loopOn] = 1.0f;
    values[Sampler::loopStart] = 0.0f;
    values[Sampler::loopEnd] = 1.0f;

    // How far the note is from a sine at its pitch: for one, each sample and the one two before it add up to the one
    // between them times 2 cos (w).
    const auto furthestFromSine = [&] (const std::shared_ptr<Sampler::Sound>& sound, int note)
    {
        auto sampler = makeSampler (sound);
        const auto played = play (*sampler, values, (int) (1.5 * testRate), { noteOn (0, note) });
        const auto turn = 2.0 * std::cos (twoPi * frequency * std::exp2 ((note - 60) / 12.0) / testRate);
        auto furthest = 0.0;

        for (size_t i = 2002; i < played.size(); ++i)
            furthest = std::max (furthest, std::abs ((double) played[i] + played[i - 2] - turn * played[i - 1]));

        CHECK (peakOf (played, played.size() - 2000, played.size()) > 0.45f); // still playing
        return furthest;
    };

    const auto seamless = make (true), plain = make (false);

    // At its own pitch, between its samples, and an octave and more up, where it reads its octaves.
    for (const auto note : { 60, 53, 67, 72, 79, 91, 96, 103 })
    {
        CAPTURE (note);
        CHECK (furthestFromSine (seamless, note) < 0.0005);
    }

    // The same sound not said to be seamless reads silence past its end, and jumps there.
    CHECK (furthestFromSine (plain, 67) > 0.02);
    CHECK (furthestFromSine (plain, 79) > 0.02);

    // Its octaves are seamless too: each a sine of twice as many cycles a sample, to its very ends.
    for (const auto& octave : seamless->octaves)
    {
        const auto size = octave.getNumSamples();
        const auto* samples = octave.getReadPointer (0);
        const auto turn = 2.0 * std::cos (twoPi * cycles / size);
        CHECK (std::abs ((double) samples[1] + samples[size - 1] - turn * samples[0]) < 0.002);
        CHECK (std::abs ((double) samples[0] + samples[size - 2] - turn * samples[size - 1]) < 0.002);
    }
}

TEST_CASE ("Wide Saw, the factory sound made from its spectrum: a saw's harmonics in bands, one seamless loop, wide")
{
    const auto names = devices::getFactorySampleNames();
    const auto index = devices::findFactorySample ("Wide Saw");
    REQUIRE (index == 1); // after the 808 bass, before the drums
    CHECK (devices::getNumPitchedFactorySamples() == 2);
    CHECK (names[(size_t) devices::getNumPitchedFactorySamples()] == std::string_view ("Kick"));
    CHECK (devices::getFactorySampleRoot (index) == 57);         // A3
    REQUIRE (devices::getFactorySampleLoop (index).has_value()); // the whole of it
    CHECK (devices::getFactorySampleLoop (index)->first == 0.0f);
    CHECK (devices::getFactorySampleLoop (index)->second == 1.0f);
    CHECK_FALSE (devices::getFactorySampleLoop (0).has_value());
    CHECK_FALSE (devices::getFactorySampleLoop (devices::findFactorySample ("808 Kick")).has_value());

    const auto started = juce::Time::getMillisecondCounterHiRes();
    const auto sound = devices::getFactorySample (index);
    MESSAGE ("Wide Saw made in " << juce::Time::getMillisecondCounterHiRes() - started << " ms");
    CHECK (devices::getFactorySample (index) == sound); // made once
    const auto& audio = sound->audio;
    const auto size = audio.getNumSamples();
    REQUIRE (audio.getNumChannels() == 2);
    REQUIRE (size == 1 << 18);
    CHECK (sound->sampleRate == 48000.0);
    CHECK (sound->seamless);
    REQUIRE (sound->onsets.size() == 1);
    CHECK (sound->onsets[0].sample == 0);

    // Each side at an RMS of -18 dB, its peaks about 12 dB over that; the right is the left from half-way round, and the
    // two are as good as unrelated.
    auto both = 0.0;

    for (int channel = 0; channel < 2; ++channel)
    {
        CHECK (juce::Decibels::gainToDecibels (audio.getRMSLevel (channel, 0, size)) == doctest::Approx (-18.0f).epsilon (0.005));
        CHECK (juce::Decibels::gainToDecibels (audio.getMagnitude (channel, 0, size)) < -3.0f);
        CHECK (juce::Decibels::gainToDecibels (audio.getMagnitude (channel, 0, size)) > -9.0f);
    }

    for (int i = 0; i < size; ++i)
        both += (double) audio.getSample (0, i) * audio.getSample (1, i);

    const auto power = (double) audio.getRMSLevel (0, 0, size) * audio.getRMSLevel (1, 0, size) * size;
    CHECK (std::abs (both / power) < 0.03);

    for (const auto i : { 0, 1, 1000, size / 2 - 1, size / 2, size - 1 })
        CHECK (audio.getSample (1, i) == audio.getSample (0, (i + size / 2) % size));

    // Its spectrum: the loop is one period, so its transform holds all of it. Each harmonic of 220 Hz at 1/n, in a band
    // whose power has a standard deviation of 4.8 cents, the same at every harmonic.
    juce::dsp::FFT fft (18);
    std::vector<float> data ((size_t) size * 2, 0.0f);
    std::copy_n (audio.getReadPointer (0), size, data.begin());
    fft.performRealOnlyForwardTransform (data.data(), true);
    const auto binHertz = 48000.0 / size;

    struct Band
    {
        double power = 0.0, cents = 0.0;
    };

    const auto measure = [&] (int harmonic, double reachHertz = 0.0)
    {
        const auto centre = 220.0 * harmonic, reach = reachHertz > 0.0 ? reachHertz : 0.02 * centre;
        auto total = 0.0, spread = 0.0;

        for (auto bin = (int) std::ceil ((centre - reach) / binHertz); bin <= (int) std::floor ((centre + reach) / binHertz); ++bin)
        {
            const auto each =
                (double) data[(size_t) bin * 2] * data[(size_t) bin * 2] + (double) data[(size_t) bin * 2 + 1] * data[(size_t) bin * 2 + 1];
            const auto cents = 1200.0 * std::log2 (bin * binHertz / centre);
            total += each;
            spread += each * cents * cents;
        }

        return Band { total, std::sqrt (spread / total) };
    };

    const auto first = measure (1);
    CHECK (first.cents == doctest::Approx (4.8).epsilon (0.02));

    for (const auto harmonic : { 2, 3, 4, 5, 8, 12, 20 })
    {
        CAPTURE (harmonic);
        const auto band = measure (harmonic);
        CHECK (10.0 * std::log10 (band.power / first.power) == doctest::Approx (-20.0 * std::log10 ((double) harmonic)).epsilon (0.005));
        CHECK (band.cents == doctest::Approx (4.8).epsilon (0.02));
    }

    // Up where the bands run into each other, half-way to each neighbour holds a harmonic's power still: to the top.
    for (const auto harmonic : { 50, 100 })
    {
        CAPTURE (harmonic);
        CHECK (10.0 * std::log10 (measure (harmonic, 110.0).power / first.power)
               == doctest::Approx (-20.0 * std::log10 ((double) harmonic)).epsilon (0.02));
    }

    // Its octaves, for notes an octave and more up: each half as long, and seamless as it is.
    auto longer = size;

    for (const auto& octave : sound->octaves)
    {
        REQUIRE (octave.getNumSamples() == longer / 2);
        longer = octave.getNumSamples();
        const auto* samples = octave.getReadPointer (0);
        auto sharpest = 0.0f;

        for (int i = 2; i < longer; ++i)
            sharpest = std::max (sharpest, std::abs (samples[i] - 2.0f * samples[i - 1] + samples[i - 2]));

        CHECK (std::abs (samples[1] - 2.0f * samples[0] + samples[longer - 1]) <= sharpest);
        CHECK (std::abs (samples[0] - 2.0f * samples[longer - 1] + samples[longer - 2]) <= sharpest);
    }

    // Looped and played once round, two octaves up, it is as wide: the left has as good as nothing to do with the right.
    auto sampler = makeSampler (sound);
    auto values = getDefaults();
    values[Sampler::root] = 57.0f;
    values[Sampler::loopOn] = 1.0f;
    values[Sampler::loopStart] = 0.0f;
    values[Sampler::loopEnd] = 1.0f;
    const auto [left, right] = playBoth (*sampler, values, size / 4, { noteOn (0, 81) });
    auto across = 0.0, leftPower = 0.0, rightPower = 0.0;

    for (size_t i = 0; i < left.size(); ++i)
    {
        across += (double) left[i] * right[i];
        leftPower += (double) left[i] * left[i];
        rightPower += (double) right[i] * right[i];
    }

    CHECK (leftPower > 1.0);
    CHECK (rightPower == doctest::Approx (leftPower).epsilon (0.05));
    CHECK (std::abs (across / std::sqrt (leftPower * rightPower)) < 0.05);
}

TEST_CASE ("Wide Saw plucked with Key Time, Filter Velocity and Start Random plays alike at any sample rate and block size")
{
    const auto sound = devices::getFactorySample (devices::findFactorySample ("Wide Saw"));
    auto values = getDefaults();
    values[Sampler::root] = 57.0f;
    values[Sampler::loopOn] = 1.0f;
    values[Sampler::loopStart] = 0.0f;
    values[Sampler::loopEnd] = 1.0f;
    values[Sampler::startRandom] = 1.0f;
    values[Sampler::keyTime] = 1.0f;
    values[Sampler::attack] = 20.0f;
    values[Sampler::decay] = 400.0f;
    values[Sampler::sustain] = 0.0f;
    values[Sampler::filterOn] = 1.0f;
    values[Sampler::cutoff] = 6000.0f;
    values[Sampler::filterVelocity] = 0.5f;

    // Notes up and down the keys, a tenth of a second apart, at every velocity: how loud each tenth of a second is.
    const auto play = [&] (double rate, int blockSize)
    {
        Sampler sampler;
        sampler.prepare (rate, blockSize);
        sampler.setSound (sound);
        const auto numSamples = (int) (2.0 * rate);
        std::vector<NoteEvent> events;

        for (int i = 0; i < 16; ++i)
        {
            const auto at = (int) (i * 0.1 * rate);
            events.push_back (noteOn (at, 33 + (i * 17) % 72, 0.2f + 0.05f * (float) i));
            events.push_back (noteOff (at + (int) (0.09 * rate), 33 + (i * 17) % 72));
        }

        juce::AudioBuffer<float> buffer (2, blockSize);
        std::vector<double> levels (20, 0.0);
        std::vector<NoteEvent> inBlock;
        auto finite = true;

        for (int done = 0; done < numSamples; done += blockSize)
        {
            const auto length = std::min (blockSize, numSamples - done);
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
            sampler.process (block);

            for (int channel = 0; channel < 2; ++channel)
            {
                for (int i = 0; i < length; ++i)
                {
                    const auto sample = buffer.getSample (channel, i);
                    finite = finite && std::isfinite (sample);
                    levels[(size_t) std::min (19, (int) ((done + i) / (0.1 * rate)))] += (double) sample * sample / rate;
                }
            }
        }

        CHECK (finite);
        CHECK (sampler.getActiveVoices() == 0); // every note has died away
        return levels;
    };

    // The same notes from the same places at every rate and block size: each tenth of a second as loud, within a decibel.
    const auto reference = play (48000.0, 256);
    CHECK (reference[0] > 1.0e-5);

    for (const auto& [rate, blockSize] : { std::pair { 44100.0, 64 }, std::pair { 48000.0, 1 }, std::pair { 48000.0, 37 },
                                           std::pair { 96000.0, 512 }, std::pair { 192000.0, 1024 } })
    {
        CAPTURE (rate);
        CAPTURE (blockSize);
        const auto levels = play (rate, blockSize);

        for (size_t i = 0; i < 16; ++i)
        {
            CAPTURE (i);
            CHECK (std::abs (10.0 * std::log10 (levels[i] / reference[i])) < 1.0);
        }
    }
}
