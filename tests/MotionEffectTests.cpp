// SPDX-License-Identifier: AGPL-3.0-or-later
// The phaser's, the flanger's and the chorus's DSP (src/devices/motion/): their notches where their responses say and as
// deep, what they play matching those responses, their sweeps and the right side's lead, the chorus's three copies a side
// as an ensemble, feedback that rings without running away, the flanger's sign and the chorus's voices turning without a
// click, Safe Bass (the phaser's and the flanger's split, as their runners play them), and their LFOs in time with the song,
// the same at each render. The chorus's voices, their phase, spread and shimmer, and its split, are in ChorusTests.cpp.
#include "MotionTestHelpers.h"
#include "TestHelpers.h"
#include "devices/dsp/Filters.h"
#include "devices/motion/Chorus.h"
#include "devices/motion/Flanger.h"
#include "devices/motion/Phaser.h"

#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

namespace
{
namespace devices = phazite::devices;
using devices::Chorus;
using devices::Flanger;
using devices::Phaser;

constexpr double rate = 48000.0;
constexpr int blockSize = 480; // 10 ms
constexpr double twoPi = juce::MathConstants<double>::twoPi;

std::vector<float> getDefaults (const devices::DeviceInfo& info)
{
    std::vector<float> values;

    for (const auto& parameter : info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

// What the song does while a sound plays: its tempo, and from which beat it plays (if it does), in 4/4.
struct Song
{
    bool playing = false;
    double startBeat = 0.0, tempo = 120.0;
};

// Plays a sound (the same on both sides unless given its right side) through a processor, in blocks of 10 ms; after each
// block, `afterBlock` is called with the block's number.
juce::AudioBuffer<float> play (devices::Processor& processor, const std::vector<float>& values, const std::vector<float>& left,
                               const std::vector<float>* right = nullptr, Song song = {}, const std::function<void (int)>& afterBlock = {})
{
    const auto numSamples = (int) left.size();
    juce::AudioBuffer<float> out (2, numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        out.setSample (0, i, left[(size_t) i]);
        out.setSample (1, i, right != nullptr ? (*right)[(size_t) i] : left[(size_t) i]);
    }

    for (int start = 0, number = 0; start < numSamples; start += blockSize, ++number)
    {
        const auto length = std::min (blockSize, numSamples - start);
        float* channels[] = { out.getWritePointer (0, start), out.getWritePointer (1, start) };
        devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = length;
        block.parameters = values;
        block.tempo = song.tempo;
        block.isPlaying = song.playing;
        block.beat = song.startBeat + start / rate * song.tempo / 60.0;
        block.bars = block.beat / 4.0; // in 4/4
        processor.process (block);

        if (afterBlock)
            afterBlock (number);
    }

    return out;
}

std::vector<float> makeSine (double hertz, int numSamples, float amplitude = 0.5f)
{
    std::vector<float> sound ((size_t) numSamples);

    for (int i = 0; i < numSamples; ++i)
        sound[(size_t) i] = amplitude * (float) std::sin (twoPi * hertz * i / rate);

    return sound;
}

std::vector<float> makeNoise (int numSamples, int seed = 1)
{
    std::vector<float> sound ((size_t) numSamples);
    juce::Random random (seed);

    for (auto& sample : sound)
        sample = 0.5f * (random.nextFloat() * 2.0f - 1.0f);

    return sound;
}

// How strong a frequency is in the left channel of a sound over its second half (its amplitude), from one bin of a
// Fourier transform over whole cycles.
double amplitudeAt (const juce::AudioBuffer<float>& sound, double hertz)
{
    const auto half = sound.getNumSamples() / 2;
    const auto cycles = std::floor (half * hertz / rate);
    const auto length = std::max (1, (int) std::lround (cycles * rate / hertz));
    auto re = 0.0, im = 0.0;

    for (int i = 0; i < length; ++i)
    {
        const auto sample = (double) sound.getSample (0, half + i);
        re += sample * std::cos (twoPi * hertz * (half + i) / rate);
        im += sample * std::sin (twoPi * hertz * (half + i) / rate);
    }

    return 2.0 * std::sqrt (re * re + im * im) / length;
}

double toDecibels (double gain)
{
    return 20.0 * std::log10 (std::max (gain, 1.0e-9));
}

// The frequencies, between 20 Hz and 20 kHz, where a response has a notch: a dip below -20 dB.
std::vector<double> findNotches (const std::function<double (double)>& magnitudeAt)
{
    std::vector<double> notches;
    constexpr int steps = 8000;
    const auto frequencyAt = [] (int step) { return 20.0 * std::pow (1000.0, (double) step / steps); };
    auto before = magnitudeAt (frequencyAt (0)), now = magnitudeAt (frequencyAt (1));

    for (int step = 1; step < steps; ++step)
    {
        const auto next = magnitudeAt (frequencyAt (step + 1));

        if (now < before && now <= next && now < 0.1)
            notches.push_back (frequencyAt (step));

        before = now;
        now = next;
    }

    return notches;
}

bool isFiniteAndBelow (const juce::AudioBuffer<float>& sound, float limit)
{
    for (int channel = 0; channel < sound.getNumChannels(); ++channel)
        for (int i = 0; i < sound.getNumSamples(); ++i)
            if (! std::isfinite (sound.getSample (channel, i)) || std::abs (sound.getSample (channel, i)) > limit)
                return false;

    return true;
}
} // namespace

TEST_CASE ("The phaser and the flanger describe themselves, with a mix, in time with the song at first")
{
    for (const auto* info : { &Phaser::info, &Flanger::info })
    {
        INFO (std::string (info->typeID));
        CHECK (devices::isValid (*info));
        CHECK (devices::findParameter (*info, devices::mixParameterID) >= 0);
        CHECK (info->parameters[(size_t) devices::findParameter (*info, "sync")].defaultValue == doctest::Approx (1.0f));
    }

    CHECK (std::string_view (Phaser::typeID) != std::string_view (Flanger::typeID));
}

TEST_CASE ("The phaser's notches: as many as asked, as deep as nothing without feedback, closer together with less spacing")
{
    auto values = getDefaults (Phaser::info);
    values[Phaser::depth] = 0.0f;
    values[Phaser::feedback] = 0.0f;

    for (int notches = 1; notches <= Phaser::maxNotches; ++notches)
    {
        CAPTURE (notches);
        values[Phaser::notches] = (float) (notches - 1);
        const auto found = findNotches ([&] (double hertz) { return Phaser::getMagnitude (values, 800.0, hertz, rate); });
        CHECK ((int) found.size() == notches);
    }

    // Two notches, either side of the centre: the classic.
    values[Phaser::notches] = 1.0f;
    const auto apart = [&] (float spacing)
    {
        values[Phaser::spacing] = spacing;
        const auto found = findNotches ([&] (double hertz) { return Phaser::getMagnitude (values, 800.0, hertz, rate); });
        REQUIRE (found.size() == 2);
        CHECK (found[0] < 800.0);
        CHECK (found[1] > 800.0);
        return std::log2 (found[1] / found[0]);
    };

    CHECK (apart (0.0f) < apart (0.5f));
    CHECK (apart (0.5f) < apart (1.0f));
    CHECK (apart (0.5f) == doctest::Approx (2.54).epsilon (0.03)); // second-order all-passes with a Q of a half

    // Between the notches, where the copy is in phase, the sound keeps its level.
    values[Phaser::spacing] = 0.5f;
    CHECK (Phaser::getMagnitude (values, 800.0, 800.0, rate) == doctest::Approx (1.0).epsilon (0.01));
}

TEST_CASE ("The phaser plays what its response says, with its feedback solved within the sample")
{
    Phaser phaser;
    auto values = getDefaults (Phaser::info);
    values[Phaser::depth] = 0.0f; // the notches standing still at the frequency
    values[Phaser::frequency] = 1000.0f;

    for (const auto feedback : { 0.0f, 0.5f, -0.6f, 0.85f })
    {
        values[Phaser::feedback] = feedback;

        for (const auto hertz : { 120.0, 400.0, 1000.0, 2500.0, 9000.0 })
        {
            CAPTURE (feedback);
            CAPTURE (hertz);
            phaser.prepare (rate, blockSize);
            const auto expected = Phaser::getMagnitude (values, 1000.0, hertz, rate);
            const auto played = amplitudeAt (play (phaser, values, makeSine (hertz, 48000)), hertz) / 0.5;
            CHECK (toDecibels (played) == doctest::Approx (toDecibels (expected)).epsilon (0.02));
        }
    }

    // Without feedback, a notch takes the sound away.
    values[Phaser::feedback] = 0.0f;
    const auto notches = findNotches ([&] (double hertz) { return Phaser::getMagnitude (values, 1000.0, hertz, rate); });
    REQUIRE (notches.size() == 2);
    phaser.prepare (rate, blockSize);
    CHECK (toDecibels (amplitudeAt (play (phaser, values, makeSine (notches[0], 48000)), notches[0]) / 0.5) < -30.0);
}

TEST_CASE ("The phaser's feedback rings between the notches, or at them with its sign turned, and never runs away")
{
    auto values = getDefaults (Phaser::info);
    values[Phaser::feedback] = 0.0f;
    const auto notch = findNotches ([&] (double hertz) { return Phaser::getMagnitude (values, 800.0, hertz, rate); }).front();
    const auto at = [&] (float feedback, double hertz)
    {
        values[Phaser::feedback] = feedback;
        return Phaser::getMagnitude (values, 800.0, hertz, rate);
    };

    // The peak between the notches rises as the feedback does.
    CHECK (at (0.3f, 800.0) > at (0.0f, 800.0));
    CHECK (at (0.85f, 800.0) > 3.0 * at (0.0f, 800.0));

    // Below 0, the notches fill in, then ring.
    CHECK (at (0.0f, notch) < 0.01);
    CHECK (at (-0.3f, notch) > 0.1);
    CHECK (at (-0.85f, notch) > 2.0);

    // At its most, swept as fast and as far as it goes, it stays within bounds.
    values[Phaser::notches] = (float) (Phaser::maxNotches - 1);
    values[Phaser::depth] = 1.0f;
    values[Phaser::sync] = 0.0f;
    values[Phaser::rate] = 20.0f;

    for (const auto feedback : { -0.85f, 0.85f })
    {
        CAPTURE (feedback);
        values[Phaser::feedback] = feedback;
        Phaser phaser;
        phaser.prepare (rate, blockSize);
        CHECK (isFiniteAndBelow (play (phaser, values, makeNoise (96000)), 8.0f));
    }

    // Its tail is longer with more feedback.
    Phaser phaser;
    phaser.prepare (rate, blockSize);
    values[Phaser::feedback] = 0.1f;
    play (phaser, values, makeNoise (480));
    const auto shortTail = phaser.getTailSeconds();
    values[Phaser::feedback] = -0.8f;
    play (phaser, values, makeNoise (480));
    CHECK (phaser.getTailSeconds() > shortTail);
}

TEST_CASE ("The phaser sweeps up to two octaves each way, the right side a part of a cycle ahead")
{
    Phaser phaser;
    phaser.prepare (rate, blockSize);
    auto values = getDefaults (Phaser::info);
    values[Phaser::sync] = 0.0f;
    values[Phaser::rate] = 5.0f;
    values[Phaser::depth] = 1.0f;
    values[Phaser::stereo] = 1.0f; // half a cycle: the sides opposite
    values[Phaser::frequency] = 800.0f;

    auto lowest = 1.0e9, highest = 0.0, widestMismatch = 0.0;
    play (phaser, values, makeNoise (48000), nullptr, {},
          [&] (int)
          {
              const auto left = (double) phaser.getCentre (0).get(), right = (double) phaser.getCentre (1).get();
              lowest = std::min (lowest, left);
              highest = std::max (highest, left);
              widestMismatch = std::max (widestMismatch, std::abs (std::log2 (left * right / (800.0 * 800.0))));
          });

    CHECK (lowest == doctest::Approx (200.0).epsilon (0.03));
    CHECK (highest == doctest::Approx (3200.0).epsilon (0.03));
    CHECK (widestMismatch < 0.02);

    // At half depth, an octave each way.
    values[Phaser::depth] = 0.5f;
    CHECK (Phaser::getSweptCentre (values, 1.0f, rate) == doctest::Approx (1600.0));
    CHECK (Phaser::getSweptCentre (values, -1.0f, rate) == doctest::Approx (400.0));
}

TEST_CASE ("The phaser's and the flanger's Safe Bass leave what is below it as it was")
{
    // The phaser's notches low, at 100 Hz: the lower one, near 41 Hz, goes when the bass isn't kept.
    auto phaserValues = getDefaults (Phaser::info);
    phaserValues[Phaser::depth] = 0.0f;
    phaserValues[Phaser::feedback] = 0.0f;
    phaserValues[Phaser::frequency] = 100.0f;
    const auto lowNotch = findNotches ([&] (double hertz) { return Phaser::getMagnitude (phaserValues, 100.0, hertz, rate); }).front();
    CHECK (Phaser::getMagnitude (phaserValues, 100.0, lowNotch, rate) < 0.05);
    phaserValues[Phaser::safeBass] = 300.0f;
    CHECK (Phaser::getMagnitude (phaserValues, 100.0, lowNotch, rate) > 0.9);

    Phaser phaser;
    phaser.prepare (rate, blockSize);
    CHECK (amplitudeAt (play (phaser, phaserValues, makeSine (lowNotch, 96000)), lowNotch) / 0.5 > 0.9);

    // The flanger with its sign turned hollows out the bass, all of it: unless it is kept.
    auto flangerValues = getDefaults (Flanger::info);
    flangerValues[Flanger::depth] = 0.0f;
    flangerValues[Flanger::feedback] = -0.5f;
    CHECK (Flanger::getMagnitude (flangerValues, 3.0, 40.0, rate) < 0.3);
    flangerValues[Flanger::safeBass] = 200.0f;
    CHECK (Flanger::getMagnitude (flangerValues, 3.0, 40.0, rate) > 0.85);

    Flanger flanger;
    flanger.prepare (rate, blockSize);
    CHECK (amplitudeAt (play (flanger, flangerValues, makeSine (40.0, 96000)), 40.0) / 0.5 > 0.85);
}

TEST_CASE ("The flanger's comb: notches at the odd multiples of half the delay's frequency, or with its sign turned the others")
{
    auto values = getDefaults (Flanger::info);
    values[Flanger::depth] = 0.0f;
    values[Flanger::feedback] = 0.0f;

    // A copy 2 ms behind: notches at 250 Hz, 750 Hz...; peaks at 500 Hz, 1 kHz...
    for (const auto hertz : { 250.0, 750.0, 1250.0 })
        CHECK (Flanger::getMagnitude (values, 2.0, hertz, rate) < 0.01);

    for (const auto hertz : { 500.0, 1000.0 })
        CHECK (Flanger::getMagnitude (values, 2.0, hertz, rate) == doctest::Approx (1.0).epsilon (0.01));

    // Turned: notches at 500 Hz and 1 kHz, as deep at any feedback.
    for (const auto feedback : { -0.2f, -0.6f, -1.0f })
    {
        values[Flanger::feedback] = feedback;
        CHECK (Flanger::getMagnitude (values, 2.0, 500.0, rate) < 0.01);
        CHECK (Flanger::getMagnitude (values, 2.0, 1000.0, rate) < 0.01);
    }

    // Feedback raises its peaks by up to 8.5 dB.
    values[Flanger::feedback] = 1.0f;
    CHECK (toDecibels (Flanger::getMagnitude (values, 2.0, 500.0, rate)) == doctest::Approx (8.5).epsilon (0.02));
    CHECK (Flanger::getMagnitude (values, 2.0, 250.0, rate) < 0.01);
}

TEST_CASE ("The flanger plays what its response says, and rings no longer than its tail")
{
    auto values = getDefaults (Flanger::info);
    values[Flanger::depth] = 0.0f; // the copy standing still at the delay
    values[Flanger::delay] = 2.37f;

    for (const auto feedback : { 0.0f, 0.6f, -0.6f })
    {
        values[Flanger::feedback] = feedback;

        for (const auto hertz : { 100.0, 211.0, 422.0, 1500.0, 5000.0 })
        {
            CAPTURE (feedback);
            CAPTURE (hertz);
            Flanger flanger;
            flanger.prepare (rate, blockSize);
            const auto expected = Flanger::getMagnitude (values, 2.37, hertz, rate);
            const auto played = amplitudeAt (play (flanger, values, makeSine (hertz, 48000)), hertz) / 0.5;

            if (expected < 0.03)
                CHECK (played < 0.03);
            else
                CHECK (toDecibels (played) == doctest::Approx (toDecibels (expected)).epsilon (0.03));
        }
    }

    // After the sound stops, what rings has fallen by 60 dB (and a little more: the delay a sample takes through the copy)
    // by the end of its tail.
    values[Flanger::feedback] = 0.9f;
    Flanger flanger;
    flanger.prepare (rate, blockSize);
    auto sound = makeNoise (4800);
    sound.resize (96000, 0.0f);
    const auto out = play (flanger, values, sound);
    const auto tailEnd = 4800 + (int) std::ceil (flanger.getTailSeconds() * rate);
    REQUIRE (tailEnd < out.getNumSamples());
    CHECK (out.getMagnitude (0, tailEnd, out.getNumSamples() - tailEnd) < 0.002f * out.getMagnitude (0, 0, 4800 + blockSize));
}

TEST_CASE ("The flanger sweeps its delay down by up to five octaves, the right side ahead, and turns its sign without a click")
{
    Flanger flanger;
    flanger.prepare (rate, blockSize);
    auto values = getDefaults (Flanger::info);
    values[Flanger::sync] = 0.0f;
    values[Flanger::rate] = 5.0f;
    values[Flanger::shape] = 0.0f; // a sine
    values[Flanger::depth] = 1.0f;
    values[Flanger::delay] = 4.0f;
    values[Flanger::stereo] = 1.0f;

    auto shortest = 1.0e9, longest = 0.0, widestMismatch = 0.0;
    play (flanger, values, makeNoise (48000), nullptr, {},
          [&] (int)
          {
              const auto left = (double) flanger.getDelay (0).get(), right = (double) flanger.getDelay (1).get();
              shortest = std::min (shortest, left);
              longest = std::max (longest, left);

              // Opposite sides of the sweep: as many octaves below the top, together, as a whole sweep.
              widestMismatch = std::max (widestMismatch, std::abs (std::log2 (left / 4.0) + std::log2 (right / 4.0) + 5.0));
          });

    CHECK (longest == doctest::Approx (4.0).epsilon (0.03));
    CHECK (shortest == doctest::Approx (4.0 / 32.0).epsilon (0.05));
    CHECK (widestMismatch < 0.05);
    CHECK (Flanger::getSweptDelay (values, 0.0f) == doctest::Approx (4.0 / std::sqrt (32.0)));

    // Feedback from one side of 0 to the other, while a tone plays: the comb glides, so no sample jumps from the one before
    // by much more than it does while the feedback stays (a click would jump by about as much as the tone is loud).
    values[Flanger::depth] = 0.0f;
    values[Flanger::feedback] = 0.7f;
    flanger.prepare (rate, blockSize);
    const auto out = play (flanger, values, makeSine (300.0, 48000), nullptr, {},
                           [&] (int block)
                           {
                               if (block == 19)
                                   values[Flanger::feedback] = -0.7f;
                           });
    const auto largestStep = [&out] (int from, int to)
    {
        auto largest = 0.0f;

        for (int i = from; i < to; ++i)
            largest = std::max (largest, std::abs (out.getSample (0, i) - out.getSample (0, i - 1)));

        return largest;
    };

    const auto steady = std::max (largestStep (4800, 9600), largestStep (38400, 48000));
    CHECK (largestStep (9600, 14400) < 3.0f * steady);

    // At its most, swept fast, it stays within bounds.
    values[Flanger::depth] = 1.0f;
    values[Flanger::rate] = 20.0f;
    values[Flanger::delay] = 15.0f;

    for (const auto feedback : { -1.0f, 1.0f })
    {
        CAPTURE (feedback);
        values[Flanger::feedback] = feedback;
        flanger.prepare (rate, blockSize);
        CHECK (isFiniteAndBelow (play (flanger, values, makeNoise (96000)), 8.0f));
    }
}

TEST_CASE ("The phaser's and the flanger's LFOs follow the song while it plays: on the beat, and the same at each render")
{
    // The flanger at two bars a cycle: from beat 2, a quarter of the way round.
    Flanger flanger;
    flanger.prepare (rate, blockSize);
    auto values = getDefaults (Flanger::info);
    const auto oneBlock = makeNoise (blockSize);
    play (flanger, values, oneBlock, nullptr, { .playing = true, .startBeat = 2.0 });
    const auto perBlock = blockSize / rate * 2.0 / 8.0; // a cycle is 8 beats; 2 beats a second
    CHECK (flanger.getPhase().get() == doctest::Approx (0.25 + perBlock).epsilon (0.001));

    // The song jumps (a loop going round): the LFO glides there over 10 ms, a block.
    play (flanger, values, oneBlock, nullptr, { .playing = true, .startBeat = 6.0 });
    CHECK (flanger.getPhase().get() == doctest::Approx (0.75 + perBlock).epsilon (0.001));

    // The phaser at a bar a cycle, played twice from the same beat: the same each time.
    auto phaserValues = getDefaults (Phaser::info);
    phaserValues[Phaser::depth] = 1.0f;
    const auto noise = makeNoise (48000, 7);
    juce::AudioBuffer<float> renders[2];

    for (auto& render : renders)
    {
        Phaser phaser;
        phaser.prepare (rate, blockSize);
        render = play (phaser, phaserValues, noise, nullptr, { .playing = true, .startBeat = 3.5 });
    }

    for (int channel = 0; channel < 2; ++channel)
        CHECK (std::memcmp (renders[0].getReadPointer (channel), renders[1].getReadPointer (channel), sizeof (float) * 48000) == 0);

    // Stopped, it runs free at its rate from where it was.
    Phaser stopped;
    stopped.prepare (rate, blockSize);
    phaserValues[Phaser::sync] = 0.0f;
    phaserValues[Phaser::rate] = 2.0f;
    play (stopped, phaserValues, makeNoise (12000));
    CHECK (stopped.getPhase().get() == doctest::Approx (0.5).epsilon (0.001)); // a quarter of a second: half a cycle
}

//==============================================================================
// Safe Bass, the phaser's and the flanger's (the chorus's is in ChorusTests.cpp): the sound split at its frequency, the
// effect hearing only the band above it, 24 dB an octave down below it, and the band below going around the effect; the
// two bands, and what the runner mixes back in, adding up flat.
namespace
{
// The phaser or the flanger, by its values' size, played through its runner (as Phazite's engine plays it, with its mix).
juce::AudioBuffer<float> playEffect (const std::vector<float>& values, const std::vector<float>& sound,
                                     const std::function<void (int, std::vector<float>&)>& beforeBlock = {})
{
    if (values.size() == Phaser::info.parameters.size())
        return phazite::test::playThroughRunner (Phaser::info, std::make_unique<Phaser>(), values, sound, rate, blockSize, beforeBlock);

    return phazite::test::playThroughRunner (Flanger::info, std::make_unique<Flanger>(), values, sound, rate, blockSize, beforeBlock);
}

// What a Linkwitz-Riley split at a frequency sends on above it, and what is left of the sound once a 12 dB Butterworth
// low-pass is taken away (which falls only 6 dB an octave below it): their gains at a frequency.
double getHighBand (double crossover, double hertz)
{
    namespace dsp = devices::dsp;
    const auto high = dsp::getResponse (dsp::makeFilter (dsp::FilterShape::highPass, crossover, rate, 0.70710678), hertz, rate);
    return std::abs (high * high);
}

double getLowTakenAway (double crossover, double hertz)
{
    namespace dsp = devices::dsp;
    return std::abs (1.0 - dsp::getResponse (dsp::makeFilter (dsp::FilterShape::lowPass, crossover, rate, 0.70710678), hertz, rate));
}

// The largest jump from one sample to the next, on either side, between two samples.
float largestJump (const juce::AudioBuffer<float>& sound, int from, int to)
{
    auto largest = 0.0f;

    for (int channel = 0; channel < sound.getNumChannels(); ++channel)
        for (int i = std::max (1, from); i < to; ++i)
            largest = std::max (largest, std::abs (sound.getSample (channel, i) - sound.getSample (channel, i - 1)));

    return largest;
}

// The phaser and the flanger each set to move the bass hard: the phaser's notches swept through it, the flanger hollowing
// it out, fast, the sides apart.
std::vector<float> getBassSweepingPhaser()
{
    auto values = getDefaults (Phaser::info);
    values[Phaser::notches] = 1.0f;
    values[Phaser::frequency] = 100.0f;
    values[Phaser::depth] = 1.0f;
    values[Phaser::sync] = 0.0f;
    values[Phaser::rate] = 3.0f;
    values[Phaser::feedback] = 0.5f;
    return values;
}

std::vector<float> getBassSweepingFlanger()
{
    auto values = getDefaults (Flanger::info);
    values[Flanger::delay] = 5.0f;
    values[Flanger::depth] = 0.5f;
    values[Flanger::sync] = 0.0f;
    values[Flanger::rate] = 3.0f;
    values[Flanger::feedback] = -0.6f;
    return values;
}
} // namespace

TEST_CASE ("The phaser's and the flanger's Safe Bass leave a 40 Hz tone under 150 Hz as it was: unmoved, the same on both sides")
{
    const auto tone = phazite::test::makeTone (40.0, 96000, rate);

    for (auto values : { getBassSweepingPhaser(), getBassSweepingFlanger() })
    {
        const auto isPhaser = values.size() == Phaser::info.parameters.size();
        const auto safeBass = isPhaser ? (size_t) Phaser::safeBass : (size_t) Flanger::safeBass;
        const auto mix = isPhaser ? (size_t) Phaser::mix : (size_t) Flanger::mix;
        CAPTURE (isPhaser);

        // Without it, each changes the tone: the phaser moves it, the flanger hollows it out.
        values[safeBass] = 20.0f;
        const auto swept = phazite::test::readTone (playEffect (values, tone), 40.0, rate, 48000, 96000);
        CHECK ((std::abs (phazite::test::inDecibels (swept.left / 0.5)) > 1.0 || phazite::test::inDecibels (swept.unsteady / 0.5) > -30.0));

        // With it at 150 Hz, at any mix: as loud as it went in, within 0.2 dB, the same on both sides, and nothing of it
        // moving, each 40 dB under the tone.
        values[safeBass] = 150.0f;

        for (const auto amount : { 1.0f, 0.5f })
        {
            CAPTURE (amount);
            values[mix] = amount;
            const auto kept = phazite::test::readTone (playEffect (values, tone), 40.0, rate, 48000, 96000);
            CHECK (std::abs (phazite::test::inDecibels (kept.left / 0.5)) < 0.2);
            CHECK (std::abs (phazite::test::inDecibels (kept.right / 0.5)) < 0.2);
            CHECK (phazite::test::inDecibels (kept.side / 0.5) < -40.0);
            CHECK (phazite::test::inDecibels (kept.unsteady / 0.5) < -40.0);
        }
    }
}

TEST_CASE ("The phaser's and the flanger's Safe Bass send their effects a 75 Hz tone under 150 Hz 24 dB down, an octave lower 48")
{
    // What of a tone reaches each one's effect: the difference between what it plays with its effect in two states that
    // treat the tone oppositely, the band around the effect being the same in both, over how far apart the two states are.
    // A Linkwitz-Riley split's: 24 dB an octave down below its frequency. At least 10 dB further down than what is left
    // once a 12 dB low-pass is taken away, which falls only 6 dB an octave (2.8 dB down at 75 Hz).
    namespace dsp = devices::dsp;

    for (const auto hertz : { 75.0, 37.5 })
    {
        CAPTURE (hertz);
        const auto tone = phazite::test::makeTone (hertz, 48000, rate);
        const auto partOf = [&] (const std::vector<float>& one, const std::vector<float>& other, double apart)
        {
            auto difference = playEffect (one, tone);
            const auto second = playEffect (other, tone);

            for (int channel = 0; channel < 2; ++channel)
                difference.addFrom (channel, 0, second, channel, 0, difference.getNumSamples(), -1.0f);

            return phazite::test::readTone (difference, hertz, rate, 24000, 48000).left / 0.5 / apart;
        };

        // The phaser's one all-pass section standing still, without feedback, at the tone (or as low as it goes) and far
        // above it: the band through it, half and half with the band's own sound, taken away or kept.
        auto phaser = getDefaults (Phaser::info);
        phaser[Phaser::notches] = 0.0f;
        phaser[Phaser::depth] = 0.0f;
        phaser[Phaser::feedback] = 0.0f;
        phaser[Phaser::stereo] = 0.0f;
        phaser[Phaser::safeBass] = 150.0f;
        auto farPhaser = phaser;
        phaser[Phaser::frequency] = (float) std::max (hertz, 50.0);
        farPhaser[Phaser::frequency] = 10000.0f;
        const auto section = [hertz] (float centre)
        { return dsp::getResponse (dsp::makeFilter (dsp::FilterShape::allPass, centre, rate, 0.5), hertz, rate); };
        const auto phaserApart = std::abs (0.5 * (section (phaser[Phaser::frequency]) - section (farPhaser[Phaser::frequency])));

        // The flanger's copy standing still, without feedback, at two delays half the tone's period apart: half the band
        // through it, delayed, upside down from one to the other.
        auto flanger = getDefaults (Flanger::info);
        flanger[Flanger::depth] = 0.0f;
        flanger[Flanger::feedback] = 0.0f;
        flanger[Flanger::stereo] = 0.0f;
        flanger[Flanger::safeBass] = 150.0f;
        auto laterFlanger = flanger;
        flanger[Flanger::delay] = 1.0f;
        laterFlanger[Flanger::delay] = (float) (1.0 + 500.0 / hertz);

        for (const auto part : { partOf (phaser, farPhaser, phaserApart), partOf (flanger, laterFlanger, 1.0) })
        {
            CHECK (phazite::test::inDecibels (part)
                   == doctest::Approx (phazite::test::inDecibels (getHighBand (150.0, hertz))).epsilon (0.02));
            CHECK (phazite::test::inDecibels (part) < phazite::test::inDecibels (getLowTakenAway (150.0, hertz)) - 10.0);
        }
    }
}

TEST_CASE ("The phaser's and the flanger's Safe Bass add back up flat: set to leave the band above it alone, they play flat")
{
    // The phaser's one all-pass section far above, narrow; the flanger's copy at its shortest, at the bottom of a slow sweep:
    // each leaves the band above Safe Bass all but alone. Then tones from two octaves under it to two over come out as loud
    // as they went in, within 0.1 dB, at any mix: the two bands add back up flat, and what the runner mixes back in with them
    // is in phase with them.
    auto phaser = getDefaults (Phaser::info);
    phaser[Phaser::notches] = 0.0f;
    phaser[Phaser::frequency] = 10000.0f;
    phaser[Phaser::depth] = 0.0f;
    phaser[Phaser::feedback] = 0.0f;
    phaser[Phaser::spacing] = 0.0f;
    phaser[Phaser::stereo] = 0.0f;
    phaser[Phaser::safeBass] = 150.0f;

    auto flanger = getDefaults (Flanger::info);
    flanger[Flanger::delay] = 0.2f;
    flanger[Flanger::depth] = 1.0f;
    flanger[Flanger::sync] = 0.0f;
    flanger[Flanger::rate] = 0.05f;
    flanger[Flanger::feedback] = 0.0f;
    flanger[Flanger::stereo] = 0.0f;
    flanger[Flanger::safeBass] = 150.0f;

    for (auto values : { phaser, flanger })
    {
        const auto isPhaser = values.size() == Phaser::info.parameters.size();
        const auto mix = isPhaser ? (size_t) Phaser::mix : (size_t) Flanger::mix;

        for (const auto amount : { 1.0f, 0.5f, 0.0f })
        {
            values[mix] = amount;

            for (int step = 0; step <= 8; ++step)
            {
                const auto hertz = 37.5 * std::exp2 (step / 2.0);
                CAPTURE (isPhaser);
                CAPTURE (amount);
                CAPTURE (hertz);
                const auto out = playEffect (values, phazite::test::makeTone (hertz, 24000, rate));
                const auto parts = phazite::test::readTone (out, hertz, rate, 9600, 24000);
                CHECK (std::abs (phazite::test::inDecibels (parts.left / 0.5)) < 0.1);
                CHECK (std::abs (phazite::test::inDecibels (parts.right / 0.5)) < 0.1);
            }
        }
    }
}

TEST_CASE ("The phaser's and the flanger's Safe Bass turn on, off and across their range without a click")
{
    // A low tone and a higher one, and Safe Bass turned on, across its range, off and on again while they play, by hand (its
    // runner gliding it over a block) and at once, from one block to the next: in the 200 ms after each change, no sample
    // jumps from the one before by much more than any does while Safe Bass stays where it is.
    std::vector<float> tones (240000);

    for (size_t i = 0; i < tones.size(); ++i)
        tones[i] = 0.25f * (float) (std::sin (twoPi * 60.0 * (double) i / rate) + std::sin (twoPi * 250.0 * (double) i / rate));

    constexpr int changes[] = { 50, 100, 150, 200 }; // blocks
    constexpr int settling = 20;                     // blocks after a change
    const auto changeAt = [] (int block, std::vector<float>& values, size_t safeBass)
    {
        for (const auto& [at, hertz] : { std::pair { 50, 150.0f }, { 100, 600.0f }, { 150, 20.0f }, { 200, 400.0f } })
            if (block == at)
                values[safeBass] = hertz;
    };

    for (auto values : { getDefaults (Phaser::info), getDefaults (Flanger::info) })
    {
        const auto isPhaser = values.size() == Phaser::info.parameters.size();
        const auto safeBass = isPhaser ? (size_t) Phaser::safeBass : (size_t) Flanger::safeBass;
        CAPTURE (isPhaser);

        const auto byHand = playEffect (values, tones, [&] (int block, std::vector<float>& now) { changeAt (block, now, safeBass); });

        std::unique_ptr<devices::Processor> effect;

        if (isPhaser)
            effect = std::make_unique<Phaser>();
        else
            effect = std::make_unique<Flanger>();

        effect->prepare (rate, blockSize);
        const auto atOnce = play (*effect, values, tones, nullptr, {}, [&] (int block) { changeAt (block + 1, values, safeBass); });

        for (const auto* out : { &byHand, &atOnce })
        {
            auto steady = largestJump (*out, 10 * blockSize, changes[0] * blockSize);

            for (size_t k = 0; k < std::size (changes); ++k)
                steady = std::max (steady, largestJump (*out, (changes[k] + settling) * blockSize,
                                                        k + 1 < std::size (changes) ? changes[k + 1] * blockSize : out->getNumSamples()));

            CHECK (steady > 0.001f);

            for (const auto change : changes)
            {
                CAPTURE (change);
                CHECK (largestJump (*out, change * blockSize, (change + settling) * blockSize) < 2.0f * steady);
            }
        }
    }
}

//==============================================================================
TEST_CASE ("The chorus describes itself: a copy a side at first, swept as the Juno-60's mode I, half and half with the sound")
{
    CHECK (devices::isValid (Chorus::info));
    auto values = getDefaults (Chorus::info);
    CHECK (values[(size_t) devices::findParameter (Chorus::info, devices::mixParameterID)] == doctest::Approx (0.5f));
    CHECK (values[Chorus::sync] == doctest::Approx (0.0f)); // in hertz at first
    CHECK (Chorus::getNumVoices (values) == 1);

    // Its triangle at the bottom of the sweep at first: the copy at the delay less its depth (1.75 ms), the right side half a
    // cycle ahead, at the top (5.25 ms).
    CHECK (Chorus::getVoiceDelay (values, Chorus::getVoicePhase (values, 0.0, 0, 0), 0) == doctest::Approx (1.75));
    CHECK (Chorus::getVoiceDelay (values, Chorus::getVoicePhase (values, 0.0, 1, 0), 0) == doctest::Approx (5.25));

    // Up to eight copies a side, picked by their number.
    values[Chorus::voices] = 2.0f;
    CHECK (Chorus::getNumVoices (values) == 3);
    values[Chorus::voices] = Chorus::info.parameters[Chorus::voices].maximum;
    CHECK (Chorus::getNumVoices (values) == Chorus::mostVoices);
    CHECK (std::string_view (Chorus::typeID) != std::string_view (Flanger::typeID));

    // Half and half with the sound, a copy 2.5 ms behind: notches at the odd multiples of half the delay's frequency (200 Hz,
    // 600 Hz...), as deep as nothing, and the sound as loud as it was between them.
    values = getDefaults (Chorus::info);
    values[Chorus::depth] = 0.0f;
    const std::vector<double> copy { 2.5 };

    for (const auto hertz : { 200.0, 600.0, 1000.0 })
        CHECK (Chorus::getMagnitude (values, copy, hertz, rate) < 0.01);

    for (const auto hertz : { 400.0, 800.0 })
        CHECK (Chorus::getMagnitude (values, copy, hertz, rate) == doctest::Approx (1.0).epsilon (0.01));
}

TEST_CASE ("The chorus plays what its response says: its copies standing still, one, three or eight a side, fed back and darkened")
{
    auto values = getDefaults (Chorus::info);
    values[Chorus::depth] = 0.0f; // the copies standing still at the delay
    values[Chorus::delay] = 2.5f; // 120 samples
    values[Chorus::mix] = 1.0f;   // its response with its copies alone, as it plays them (its runner mixes in the sound)

    for (const auto voices : { 0.0f, 2.0f, 7.0f })
    {
        for (const auto feedback : { 0.0f, 0.6f })
        {
            for (const auto highCut : { 20000.0f, 3000.0f })
            {
                values[Chorus::voices] = voices;
                values[Chorus::feedback] = feedback;
                values[Chorus::highCut] = highCut;
                const std::vector<double> copies ((size_t) Chorus::getNumVoices (values), 2.5);

                for (const auto hertz : { 100.0, 200.0, 400.0, 1500.0, 5000.0 })
                {
                    CAPTURE (voices);
                    CAPTURE (feedback);
                    CAPTURE (highCut);
                    CAPTURE (hertz);
                    Chorus chorus;
                    chorus.prepare (rate, blockSize);
                    const auto expected = Chorus::getMagnitude (values, copies, hertz, rate);
                    const auto played = amplitudeAt (play (chorus, values, makeSine (hertz, 48000)), hertz) / 0.5;
                    CHECK (toDecibels (played) == doctest::Approx (toDecibels (expected)).epsilon (0.03));
                }
            }
        }
    }

    // Three copies together where they line up (here, standing still at one delay, everywhere): 4.8 dB louder than one; and
    // eight, 9 dB.
    values[Chorus::voices] = 2.0f;
    values[Chorus::feedback] = 0.0f;
    values[Chorus::highCut] = 20000.0f;
    CHECK (toDecibels (Chorus::getMagnitude (values, std::vector<double> (3, 2.5), 1000.0, rate)) == doctest::Approx (4.77).epsilon (0.01));
    values[Chorus::voices] = 7.0f;
    CHECK (toDecibels (Chorus::getMagnitude (values, std::vector<double> (8, 2.5), 1000.0, rate)) == doctest::Approx (9.03).epsilon (0.01));
}

TEST_CASE ("The chorus sweeps its copies each way by its depth, the sides opposite, and three a side a third of a cycle apart")
{
    Chorus chorus;
    chorus.prepare (rate, blockSize);
    auto values = getDefaults (Chorus::info);
    values[Chorus::rate] = 5.0f;
    values[Chorus::shape] = 0.0f; // a sine
    values[Chorus::delay] = 4.0f;
    values[Chorus::depth] = 0.5f;

    auto shortest = 1.0e9, longest = 0.0, widestMismatch = 0.0;
    play (chorus, values, makeNoise (48000), nullptr, {},
          [&] (int)
          {
              const auto left = (double) chorus.getDelay (0, 0).get(), right = (double) chorus.getDelay (1, 0).get();
              shortest = std::min (shortest, left);
              longest = std::max (longest, left);

              // Opposite sides of the sweep: as far each way from its middle.
              widestMismatch = std::max (widestMismatch, std::abs (left + right - 8.0));
          });

    CHECK (shortest == doctest::Approx (2.0).epsilon (0.01));
    CHECK (longest == doctest::Approx (6.0).epsilon (0.01));
    CHECK (widestMismatch < 0.001);

    // As the string machines' ensemble, three copies a side a third of a cycle apart, the fast sine on each: their sweeps
    // cancel, so that together they keep to the middle, spread either way of it.
    constexpr int ensembleVoices = 3;
    values[Chorus::voices] = (float) (ensembleVoices - 1);
    values[Chorus::shimmer] = 1.0f;
    values[Chorus::rate] = 0.7f;
    chorus.prepare (rate, blockSize);
    auto worstMiddle = 0.0, widestSpread = 0.0;
    play (chorus, values, makeNoise (48000), nullptr, {},
          [&] (int)
          {
              for (int channel = 0; channel < 2; ++channel)
              {
                  auto sum = 0.0, low = 1.0e9, high = 0.0;

                  for (int voice = 0; voice < ensembleVoices; ++voice)
                  {
                      const auto lag = (double) chorus.getDelay (channel, voice).get();
                      sum += lag;
                      low = std::min (low, lag);
                      high = std::max (high, lag);
                  }

                  worstMiddle = std::max (worstMiddle, std::abs (sum / ensembleVoices - 4.0));
                  widestSpread = std::max (widestSpread, high - low);
              }
          });

    CHECK (worstMiddle < 0.001);
    CHECK (widestSpread > 2.0);
}

TEST_CASE ("The chorus's feedback never runs away, its voices change without a click, and it rings no longer than its tail")
{
    // At its most, swept fast, it stays within bounds.
    Chorus chorus;
    auto values = getDefaults (Chorus::info);
    values[Chorus::depth] = 1.0f;
    values[Chorus::rate] = 20.0f;
    values[Chorus::feedback] = 0.9f;

    for (const auto voices : { 0.0f, 2.0f, 7.0f })
    {
        for (const auto delay : { 1.0f, 25.0f })
        {
            for (const auto highCut : { 20000.0f, 2000.0f })
            {
                CAPTURE (voices);
                CAPTURE (delay);
                CAPTURE (highCut);
                values[Chorus::voices] = voices;
                values[Chorus::shimmer] = voices > 0.0f ? 1.0f : 0.0f;
                values[Chorus::delay] = delay;
                values[Chorus::highCut] = highCut;
                chorus.prepare (rate, blockSize);
                CHECK (isFiniteAndBelow (play (chorus, values, makeNoise (96000)), 8.0f));
            }
        }
    }

    // From one copy a side to three and back while a tone plays: the copies fade in and out, so no sample jumps from the one
    // before by much more than it does while their number stays.
    values = getDefaults (Chorus::info);
    chorus.prepare (rate, blockSize);
    const auto out = play (chorus, values, makeSine (300.0, 48000), nullptr, {},
                           [&] (int block)
                           {
                               if (block == 19)
                                   values[Chorus::voices] = 2.0f;

                               if (block == 59)
                                   values[Chorus::voices] = 0.0f;
                           });
    const auto largestStep = [&out] (int from, int to)
    {
        auto largest = 0.0f;

        for (int channel = 0; channel < 2; ++channel)
            for (int i = from; i < to; ++i)
                largest = std::max (largest, std::abs (out.getSample (channel, i) - out.getSample (channel, i - 1)));

        return largest;
    };

    const auto steady = std::max ({ largestStep (4800, 9600), largestStep (19200, 28800), largestStep (38400, 48000) });
    CHECK (largestStep (9600, 14400) < 3.0f * steady);
    CHECK (largestStep (28800, 33600) < 3.0f * steady);

    // After the sound stops, what rings has fallen by more than 54 dB by the end of its tail.
    values[Chorus::feedback] = 0.8f;
    values[Chorus::delay] = 10.0f;
    chorus.prepare (rate, blockSize);
    auto sound = makeNoise (4800);
    sound.resize (96000, 0.0f);
    const auto rung = play (chorus, values, sound);
    const auto tailEnd = 4800 + (int) std::ceil (chorus.getTailSeconds() * rate);
    REQUIRE (tailEnd < rung.getNumSamples());
    CHECK (rung.getMagnitude (0, tailEnd, rung.getNumSamples() - tailEnd) < 0.002f * rung.getMagnitude (0, 0, 4800 + blockSize));
}

TEST_CASE ("The chorus's Safe Bass leaves what is below it as it was, and its LFO follows the song in time with it")
{
    // Half and half with copies 12.5 ms behind: a notch at 40 Hz, which hollows out a bass note; unless the bass is kept.
    auto values = getDefaults (Chorus::info);
    values[Chorus::depth] = 0.0f;
    values[Chorus::delay] = 12.5f;
    const std::vector<double> copy { 12.5 };
    CHECK (Chorus::getMagnitude (values, copy, 40.0, rate) < 0.01);
    values[Chorus::safeBass] = 200.0f;
    CHECK (Chorus::getMagnitude (values, copy, 40.0, rate) > 0.95);

    // And it plays so: its copies alone, with the bass around them, are what its response says.
    values[Chorus::mix] = 1.0f;
    Chorus chorus;
    chorus.prepare (rate, blockSize);
    const auto expected = Chorus::getMagnitude (values, copy, 40.0, rate);
    const auto played = amplitudeAt (play (chorus, values, makeSine (40.0, 96000)), 40.0) / 0.5;
    CHECK (toDecibels (played) == doctest::Approx (toDecibels (expected)).epsilon (0.03));

    // In time with the song at a bar a cycle: from beat 2, half way round.
    values = getDefaults (Chorus::info);
    values[Chorus::sync] = 1.0f;
    chorus.prepare (rate, blockSize);
    play (chorus, values, makeNoise (blockSize), nullptr, { .playing = true, .startBeat = 2.0 });
    const auto perBlock = blockSize / rate * 2.0 / 4.0; // a cycle is 4 beats; 2 beats a second
    CHECK (chorus.getPhase().get() == doctest::Approx (0.5 + perBlock).epsilon (0.001));
}
