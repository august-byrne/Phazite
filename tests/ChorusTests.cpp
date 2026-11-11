// SPDX-License-Identifier: AGPL-3.0-or-later
// The chorus's voices (src/devices/motion/Chorus.h): up to eight copies a side, together as loud as one; Voice Phase, which
// at its most spreads the copies of both sides evenly round the LFO's cycle; Spread, which gives each copy a part of the
// sweep to itself; Shimmer, the fast sine as an amount; that one voice ignores what only several use; the number of voices
// changing without a click; feedback with many voices; the same sweep at every sample rate and block size; and Safe Bass's
// split, as its runner plays it, with its mix. What the chorus had before its voices (its response, its sweep, its
// feedback, its LFO in time) is in MotionEffectTests.cpp.
#include "MotionTestHelpers.h"
#include "TestHelpers.h"
#include "devices/dsp/Filters.h"
#include "devices/motion/Chorus.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

namespace
{
namespace devices = phazite::devices;
using devices::Chorus;

constexpr double usualRate = 48000.0;
constexpr int usualBlock = 480; // 10 ms
constexpr double twoPi = juce::MathConstants<double>::twoPi;

std::vector<float> getDefaults()
{
    std::vector<float> values;

    for (const auto& parameter : Chorus::info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

// Plays a sound, the same on both sides, through a chorus in blocks of a size; after each block, `afterBlock` is called
// with the block's number.
juce::AudioBuffer<float> play (Chorus& chorus, const std::vector<float>& values, const std::vector<float>& sound,
                               int blockSize = usualBlock, const std::function<void (int)>& afterBlock = {})
{
    const auto numSamples = (int) sound.size();
    juce::AudioBuffer<float> out (2, numSamples);

    for (int channel = 0; channel < 2; ++channel)
        std::copy (sound.begin(), sound.end(), out.getWritePointer (channel));

    for (int start = 0, number = 0; start < numSamples; start += blockSize, ++number)
    {
        float* channels[] = { out.getWritePointer (0, start), out.getWritePointer (1, start) };
        devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = std::min (blockSize, numSamples - start);
        block.parameters = values;
        chorus.process (block);

        if (afterBlock)
            afterBlock (number);
    }

    return out;
}

std::vector<float> makeSine (double hertz, int numSamples, double sampleRate = usualRate, float amplitude = 0.5f)
{
    std::vector<float> sound ((size_t) numSamples);

    for (int i = 0; i < numSamples; ++i)
        sound[(size_t) i] = amplitude * (float) std::sin (twoPi * hertz * i / sampleRate);

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

// A sound's level over its second half, on a side, in decibels.
double levelOf (const juce::AudioBuffer<float>& sound, int channel)
{
    const auto half = sound.getNumSamples() / 2;
    return 20.0 * std::log10 (std::max (1.0e-9, (double) sound.getRMSLevel (channel, half, sound.getNumSamples() - half)));
}

// The largest jump from one sample to the next, on either side, between two samples.
float largestStep (const juce::AudioBuffer<float>& sound, int from, int to)
{
    auto largest = 0.0f;

    for (int channel = 0; channel < sound.getNumChannels(); ++channel)
        for (int i = std::max (1, from); i < to; ++i)
            largest = std::max (largest, std::abs (sound.getSample (channel, i) - sound.getSample (channel, i - 1)));

    return largest;
}

bool isFiniteAndBelow (const juce::AudioBuffer<float>& sound, float limit)
{
    for (int channel = 0; channel < sound.getNumChannels(); ++channel)
        for (int i = 0; i < sound.getNumSamples(); ++i)
            if (! std::isfinite (sound.getSample (channel, i)) || std::abs (sound.getSample (channel, i)) > limit)
                return false;

    return true;
}

bool areSame (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    if (a.getNumSamples() != b.getNumSamples())
        return false;

    for (int channel = 0; channel < 2; ++channel)
        if (std::memcmp (a.getReadPointer (channel), b.getReadPointer (channel), sizeof (float) * (size_t) a.getNumSamples()) != 0)
            return false;

    return true;
}

float voicesFor (int count)
{
    return (float) (count - 1); // the choice that picks so many copies a side
}
} // namespace

TEST_CASE ("The chorus's voices: one a side at first, up to eight, described as choices of their number, the rest at rest")
{
    CHECK (devices::isValid (Chorus::info));
    const auto values = getDefaults();
    const auto& voices = Chorus::info.parameters[Chorus::voices];
    CHECK (voices.id == "voices");
    CHECK (voices.unit == devices::Unit::choice);
    CHECK ((int) voices.choices.size() == Chorus::mostVoices);
    CHECK (voices.choices.front() == "1");
    CHECK (voices.choices.back() == "8");
    CHECK (Chorus::getNumVoices (values) == 1);

    // No shimmer, the voices as far apart in the cycle as they go, each sweeping the whole range: what several voices then
    // play is the string machines' ensemble but for its shimmer.
    CHECK (values[Chorus::shimmer] == doctest::Approx (0.0f));
    CHECK (values[Chorus::voicePhase] == doctest::Approx (1.0f));
    CHECK (values[Chorus::spread] == doctest::Approx (0.0f));
    CHECK (Chorus::info.parameters[Chorus::shimmer].id == "shimmer");
    CHECK (Chorus::info.parameters[Chorus::voicePhase].id == "voicePhase");
    CHECK (Chorus::info.parameters[Chorus::spread].id == "spread");

    for (const auto index : { Chorus::shimmer, Chorus::voicePhase, Chorus::spread })
    {
        CHECK (Chorus::info.parameters[index].unit == devices::Unit::percent);
        CHECK (Chorus::info.parameters[index].minimum == doctest::Approx (0.0f));
        CHECK (Chorus::info.parameters[index].maximum == doctest::Approx (1.0f));
    }
}

TEST_CASE ("The chorus's voices together are as loud as one: on noise as loud as one copy, and where they line up louder")
{
    // One copy of noise is about as loud as what went in (a little less: reading between samples takes some of its top).
    const auto noise = makeNoise (96000);
    const auto inLevel = 20.0 * std::log10 (0.5 / std::sqrt (3.0)); // noise evenly between -0.5 and 0.5
    auto oneLevel = 0.0;

    {
        auto values = getDefaults();
        values[Chorus::delay] = 8.0f;
        values[Chorus::rate] = 0.7f;
        Chorus chorus;
        chorus.prepare (usualRate, usualBlock);
        oneLevel = levelOf (play (chorus, values, noise), 0);
        CHECK (std::abs (oneLevel - inLevel) < 1.5);
    }

    for (int count = 2; count <= Chorus::mostVoices; ++count)
    {
        for (const auto spread : { 0.0f, 0.6f, 1.0f })
        {
            CAPTURE (count);
            CAPTURE (spread);
            auto values = getDefaults();
            values[Chorus::voices] = voicesFor (count);
            values[Chorus::spread] = spread;
            values[Chorus::delay] = 8.0f;
            values[Chorus::rate] = 0.7f;
            Chorus chorus;
            chorus.prepare (usualRate, usualBlock);
            const auto out = play (chorus, values, noise); // the copies alone: its runner mixes the sound in

            for (int channel = 0; channel < 2; ++channel)
                CHECK (std::abs (levelOf (out, channel) - oneLevel) < 0.75); // decibels
        }
    }

    // Where the copies line up (here at no frequency at all, a steady value), eight are 9 dB louder than what went in.
    auto values = getDefaults();
    values[Chorus::voices] = voicesFor (8);
    values[Chorus::spread] = 0.6f;
    Chorus chorus;
    chorus.prepare (usualRate, usualBlock);
    const auto steady = play (chorus, values, std::vector<float> (24000, 0.1f));
    CHECK (steady.getSample (0, 23999) == doctest::Approx (0.1f * std::sqrt (8.0f)).epsilon (0.001));
}

TEST_CASE ("The chorus's Voice Phase: at its most the copies of both sides are spread evenly round the cycle, at none together")
{
    auto values = getDefaults();

    // At 100 %, with the sides opposite: every copy of the two sides as far from its neighbours as the others, round the
    // cycle; with three a side, a third of a cycle apart on each side, as the string machines' three lines.
    for (int count = 2; count <= Chorus::mostVoices; ++count)
    {
        CAPTURE (count);
        values[Chorus::voices] = voicesFor (count);
        std::vector<double> phases;

        for (int channel = 0; channel < 2; ++channel)
            for (int voice = 0; voice < count; ++voice)
            {
                const auto at = Chorus::getVoicePhase (values, 0.125, channel, voice);
                phases.push_back (at - std::floor (at));
            }

        std::sort (phases.begin(), phases.end());

        for (size_t i = 1; i < phases.size(); ++i)
            CHECK (phases[i] - phases[i - 1] == doctest::Approx (1.0 / (2.0 * count)).epsilon (0.0001));
    }

    values[Chorus::voices] = voicesFor (3);

    for (int voice = 0; voice < 3; ++voice)
        CHECK (Chorus::getVoicePhase (values, 0.0, 0, voice) == doctest::Approx (voice / 3.0));

    // At none, a side's copies are at one point of the cycle; the other side's still half a cycle ahead (Stereo's).
    values[Chorus::voices] = voicesFor (5);
    values[Chorus::voicePhase] = 0.0f;

    for (int voice = 0; voice < 5; ++voice)
    {
        CHECK (Chorus::getVoicePhase (values, 0.25, 0, voice) == doctest::Approx (0.25));
        CHECK (Chorus::getVoicePhase (values, 0.25, 1, voice) == doctest::Approx (0.75));
    }

    // Between, in proportion: four voices at 52.4 % are 70.8 degrees apart, as a MultiChorus set to 212.4 degrees over them.
    values[Chorus::voices] = voicesFor (4);
    values[Chorus::voicePhase] = 0.5244f;
    CHECK ((Chorus::getVoicePhase (values, 0.0, 0, 1) - Chorus::getVoicePhase (values, 0.0, 0, 0)) * 360.0
           == doctest::Approx (70.8).epsilon (0.001));

    // And it plays so: at none, a side's copies sweep together, each as far as the whole sweep.
    values = getDefaults();
    values[Chorus::voices] = voicesFor (4);
    values[Chorus::voicePhase] = 0.0f;
    values[Chorus::rate] = 5.0f;
    Chorus chorus;
    chorus.prepare (usualRate, usualBlock);
    auto widestApart = 0.0;
    play (chorus, values, makeNoise (24000), usualBlock,
          [&] (int)
          {
              for (int voice = 1; voice < 4; ++voice)
                  widestApart =
                      std::max (widestApart, std::abs ((double) chorus.getDelay (0, voice).get() - (double) chorus.getDelay (0, 0).get()));
          });
    CHECK (widestApart < 0.0001);
}

TEST_CASE ("The chorus's Spread gives each copy a part of the sweep to itself: the whole at none, end to end at its most")
{
    auto values = getDefaults();
    values[Chorus::voices] = voicesFor (4);
    values[Chorus::delay] = 8.0f;
    values[Chorus::depth] = 0.5f; // the whole sweep from 4 to 12 ms

    // At none, every copy sweeps the whole of it.
    for (int voice = 0; voice < 4; ++voice)
    {
        CHECK (Chorus::getVoiceReach (values, voice).shortest == doctest::Approx (4.0));
        CHECK (Chorus::getVoiceReach (values, voice).longest == doctest::Approx (12.0));
    }

    // At its most, the parts lie end to end: 2 ms each, the first the shortest delays.
    values[Chorus::spread] = 1.0f;

    for (int voice = 0; voice < 4; ++voice)
    {
        CHECK (Chorus::getVoiceReach (values, voice).shortest == doctest::Approx (4.0 + 2.0 * voice));
        CHECK (Chorus::getVoiceReach (values, voice).longest == doctest::Approx (6.0 + 2.0 * voice));
    }

    // Between, the parts overlap, and together still cover the whole sweep and no more. As a MultiChorus with an overlap of
    // 0.38 lays its four voices over 3.19 to 13.19 ms: 3.19 to 6.69, 5.36 to 8.85, 7.53 to 11.02 and 9.69 to 13.19 ms.
    values[Chorus::spread] = 0.62f;
    values[Chorus::delay] = 8.19f;
    values[Chorus::depth] = 0.6105f;
    const double expected[4][2] = { { 3.19, 6.69 }, { 5.36, 8.85 }, { 7.53, 11.02 }, { 9.69, 13.19 } };

    for (int voice = 0; voice < 4; ++voice)
    {
        CHECK (Chorus::getVoiceReach (values, voice).shortest == doctest::Approx (expected[voice][0]).epsilon (0.002));
        CHECK (Chorus::getVoiceReach (values, voice).longest == doctest::Approx (expected[voice][1]).epsilon (0.002));
    }

    // And it plays so: each copy stays in its part, reaching both of its ends, the sides opposite within it.
    values[Chorus::rate] = 5.0f;
    values[Chorus::shape] = 0.0f; // a sine
    Chorus chorus;
    chorus.prepare (usualRate, usualBlock);
    std::array<double, 4> shortest, longest;
    shortest.fill (1.0e9);
    longest.fill (0.0);
    auto worstOpposite = 0.0;
    play (chorus, values, makeNoise (48000), usualBlock,
          [&] (int)
          {
              for (size_t voice = 0; voice < 4; ++voice)
              {
                  const auto left = (double) chorus.getDelay (0, (int) voice).get(),
                             right = (double) chorus.getDelay (1, (int) voice).get();
                  shortest[voice] = std::min (shortest[voice], left);
                  longest[voice] = std::max (longest[voice], left);
                  worstOpposite = std::max (worstOpposite, std::abs (left + right - (expected[voice][0] + expected[voice][1])));
              }
          });

    for (size_t voice = 0; voice < 4; ++voice)
    {
        CHECK (shortest[voice] == doctest::Approx (expected[voice][0]).epsilon (0.01));
        CHECK (longest[voice] == doctest::Approx (expected[voice][1]).epsilon (0.01));
    }

    CHECK (worstOpposite < 0.02);

    // Each copy's pitch then wavers less than the whole sweep's would: by its part of it, here 1 / 2.86.
    const auto whole = 8.19 * 0.6105, part = 0.5 * (expected[0][1] - expected[0][0]);
    CHECK (part / whole == doctest::Approx (1.0 / 2.86).epsilon (0.01));
}

TEST_CASE ("The chorus's Shimmer puts a sine ten times as fast on the sweep, up to a tenth as deep, and none at none")
{
    auto values = getDefaults();
    values[Chorus::shape] = 0.0f; // a sine
    values[Chorus::delay] = 10.0f;
    values[Chorus::depth] = 0.5f;

    // How strong the sweep's tenth harmonic is, and its first, over one cycle of a copy's delay (milliseconds).
    const auto harmonicOf = [&values] (int harmonic)
    {
        constexpr int steps = 4000;
        auto re = 0.0, im = 0.0;

        for (int i = 0; i < steps; ++i)
        {
            const auto at = (double) i / steps;
            const auto lag = Chorus::getVoiceDelay (values, at, 0);
            re += lag * std::cos (twoPi * harmonic * at);
            im += lag * std::sin (twoPi * harmonic * at);
        }

        return 2.0 * std::sqrt (re * re + im * im) / steps;
    };

    CHECK (harmonicOf (1) == doctest::Approx (5.0).epsilon (0.001));
    CHECK (harmonicOf (10) < 1.0e-6);

    // At its most: a tenth as deep as the slow sweep, the two together reaching no further than the depth.
    values[Chorus::shimmer] = 1.0f;
    CHECK (harmonicOf (10) == doctest::Approx (5.0 * 0.1 / 1.1).epsilon (0.001));
    CHECK (harmonicOf (1) == doctest::Approx (5.0 / 1.1).epsilon (0.001));

    for (int i = 0; i < 1000; ++i)
        CHECK (std::abs (Chorus::getSweep (values, i / 1000.0)) <= 1.0f);

    // Half way: half as much of it.
    values[Chorus::shimmer] = 0.5f;
    CHECK (harmonicOf (10) == doctest::Approx (5.0 * 0.05 / 1.05).epsilon (0.001));

    // And it plays so, with one copy a side too: its delay as the sweep says at the LFO's phase, block by block.
    values[Chorus::rate] = 2.0f;
    Chorus chorus;
    chorus.prepare (usualRate, usualBlock);
    auto worst = 0.0;
    play (chorus, values, makeNoise (48000), usualBlock,
          [&] (int number)
          {
              const auto phase = 2.0 * number * usualBlock / usualRate; // as the block started
              worst = std::max (worst, std::abs ((double) chorus.getDelay (0, 0).get() - Chorus::getVoiceDelay (values, phase, 0)));
          });
    CHECK (worst < 0.001);
}

TEST_CASE ("The chorus's voices follow the song in time with it, each where Voice Phase and Stereo put it in the cycle")
{
    // A bar a cycle, played from beat 2: the LFO half way round as the first block starts, every copy of both sides where
    // its phase says.
    auto values = getDefaults();
    values[Chorus::voices] = voicesFor (5);
    values[Chorus::voicePhase] = 0.6f;
    values[Chorus::spread] = 0.4f;
    values[Chorus::stereo] = 0.7f;
    values[Chorus::sync] = 1.0f;
    Chorus chorus;
    chorus.prepare (usualRate, usualBlock);
    juce::AudioBuffer<float> sound (2, usualBlock);
    sound.clear();
    float* channels[] = { sound.getWritePointer (0), sound.getWritePointer (1) };
    devices::ProcessContext block;
    block.channels = channels;
    block.numChannels = 2;
    block.numSamples = usualBlock;
    block.parameters = values;
    block.tempo = 120.0;
    block.isPlaying = true;
    block.beat = 2.0;
    block.bars = 0.5; // in 4/4
    chorus.process (block);

    for (int channel = 0; channel < 2; ++channel)
        for (int voice = 0; voice < 5; ++voice)
        {
            CAPTURE (channel);
            CAPTURE (voice);
            const auto expected = Chorus::getVoiceDelay (values, Chorus::getVoicePhase (values, 0.5, channel, voice), voice);
            CHECK ((double) chorus.getDelay (channel, voice).get() == doctest::Approx (expected).epsilon (0.0001));
        }
}

TEST_CASE ("The chorus with one voice plays the same whatever Voice Phase and Spread are set to")
{
    const auto noise = makeNoise (24000);
    auto values = getDefaults();
    values[Chorus::feedback] = 0.4f;
    Chorus chorus;
    chorus.prepare (usualRate, usualBlock);
    const auto usual = play (chorus, values, noise);

    values[Chorus::voicePhase] = 0.0f;
    values[Chorus::spread] = 1.0f;
    chorus.prepare (usualRate, usualBlock);
    CHECK (areSame (play (chorus, values, noise), usual));

    // Several voices at no Spread and no Voice Phase, at no depth: all copies in one place, which is one copy as loud as
    // they are together where they line up.
    values = getDefaults();
    values[Chorus::depth] = 0.0f;
    chorus.prepare (usualRate, usualBlock);
    const auto one = play (chorus, values, noise);
    values[Chorus::voices] = voicesFor (4);
    chorus.prepare (usualRate, usualBlock);
    const auto four = play (chorus, values, noise);
    auto worst = 0.0f;

    for (int i = 0; i < 24000; ++i)
        worst = std::max (worst, std::abs (four.getSample (0, i) - 2.0f * one.getSample (0, i)));

    CHECK (worst < 1.0e-5f);
}

TEST_CASE ("The chorus's voices change in number without a click, one change at a time, with feedback and without")
{
    // A tone, and the number of copies changed every 100 ms through every count, the copies spread and fed back: no sample
    // jumps from the one before by much more than it does while their number stays.
    for (const auto feedback : { 0.0f, 0.6f })
    {
        CAPTURE (feedback);
        auto values = getDefaults();
        values[Chorus::spread] = 0.5f;
        values[Chorus::feedback] = feedback;
        values[Chorus::delay] = 8.0f;
        Chorus chorus;
        chorus.prepare (usualRate, usualBlock);
        const int counts[] = { 1, 8, 2, 7, 3, 6, 4, 5, 1 };
        const auto out = play (chorus, values, makeSine (300.0, 48000 * 2), usualBlock,
                               [&] (int block)
                               {
                                   if (block >= 49 && (block + 1) % 10 == 0)
                                       values[Chorus::voices] = voicesFor (counts[std::min ((block - 49) / 10, 8)]);
                               });

        const auto steady = largestStep (out, 4800, 24000); // one copy a side, before any change
        CHECK (steady > 0.001f);
        CHECK (largestStep (out, 24000, out.getNumSamples()) < 3.0f * steady);
        CHECK (isFiniteAndBelow (out, 4.0f));
    }

    // Changed at every block, faster than a set of copies fades: it follows as it can, without a click and within bounds.
    auto values = getDefaults();
    values[Chorus::feedback] = 0.9f;
    values[Chorus::spread] = 1.0f;
    Chorus chorus;
    chorus.prepare (usualRate, 64);
    const auto hurried =
        play (chorus, values, makeSine (300.0, 48000), 64, [&] (int block) { values[Chorus::voices] = voicesFor (1 + (block * 5) % 8); });
    CHECK (isFiniteAndBelow (hurried, 8.0f));

    values[Chorus::feedback] = 0.0f;
    chorus.prepare (usualRate, 64);
    const auto quick =
        play (chorus, values, makeSine (300.0, 48000), 64, [&] (int block) { values[Chorus::voices] = voicesFor (1 + (block * 5) % 8); });
    CHECK (largestStep (quick, 4800, 48000) < 0.15f); // the tone itself steps 0.02 a sample, eight copies in phase 0.055
}

TEST_CASE ("The chorus's feedback stays within bounds with many voices, spread or not, and its response says what it plays")
{
    // At its most, swept fast and deep, with eight copies a side: within bounds.
    Chorus chorus;
    auto values = getDefaults();
    values[Chorus::depth] = 1.0f;
    values[Chorus::rate] = 20.0f;
    values[Chorus::feedback] = 0.9f;
    values[Chorus::shimmer] = 1.0f;

    for (const auto count : { 2, 4, 8 })
    {
        for (const auto spread : { 0.0f, 0.5f, 1.0f })
        {
            for (const auto voicePhase : { 0.0f, 1.0f })
            {
                for (const auto delay : { 1.0f, 25.0f })
                {
                    CAPTURE (count);
                    CAPTURE (spread);
                    CAPTURE (voicePhase);
                    CAPTURE (delay);
                    values[Chorus::voices] = voicesFor (count);
                    values[Chorus::spread] = spread;
                    values[Chorus::voicePhase] = voicePhase;
                    values[Chorus::delay] = delay;
                    chorus.prepare (usualRate, usualBlock);
                    CHECK (isFiniteAndBelow (play (chorus, values, makeNoise (96000)), 8.0f));
                }
            }
        }
    }

    // Standing still (no depth), eight copies fed back and darkened play what the response says, Safe Bass on.
    values = getDefaults();
    values[Chorus::voices] = voicesFor (8);
    values[Chorus::depth] = 0.0f;
    values[Chorus::delay] = 2.5f;
    values[Chorus::feedback] = 0.6f;
    values[Chorus::highCut] = 3000.0f;
    values[Chorus::safeBass] = 150.0f;
    values[Chorus::mix] = 1.0f;
    const std::vector<double> copies (8, 2.5);

    for (const auto hertz : { 100.0, 400.0, 1500.0 })
    {
        CAPTURE (hertz);
        chorus.prepare (usualRate, usualBlock);
        const auto out = play (chorus, values, makeSine (hertz, 48000));
        const auto played = out.getRMSLevel (0, 24000, 24000) / (0.5 / std::sqrt (2.0));
        const auto expected = Chorus::getMagnitude (values, copies, hertz, usualRate);
        CHECK (20.0 * std::log10 (played) == doctest::Approx (20.0 * std::log10 (expected)).epsilon (0.03));
    }
}

TEST_CASE ("The chorus's voices sweep the same at every sample rate, from 44.1 to 192 kHz, and at any block size")
{
    auto values = getDefaults();
    values[Chorus::voices] = voicesFor (8);
    values[Chorus::spread] = 0.6f;
    values[Chorus::voicePhase] = 0.7f;
    values[Chorus::shimmer] = 0.5f;
    values[Chorus::delay] = 20.0f;
    values[Chorus::depth] = 1.0f; // up to 40 ms: the longest the eighth copy lags
    values[Chorus::rate] = 2.0f;
    values[Chorus::feedback] = 0.5f;
    values[Chorus::highCut] = 8000.0f;

    for (const auto sampleRate : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
    {
        CAPTURE (sampleRate);
        const auto blockSize = (int) (sampleRate / 100.0);
        const auto numSamples = blockSize * 100; // a second
        Chorus chorus;
        chorus.prepare (sampleRate, blockSize);
        auto worst = 0.0;
        const auto out = play (chorus, values, makeNoise (numSamples), blockSize,
                               [&] (int number)
                               {
                                   const auto phase = 2.0 * number / 100.0; // as the block started

                                   for (int channel = 0; channel < 2; ++channel)
                                       for (int voice = 0; voice < 8; ++voice)
                                           worst = std::max (
                                               worst, std::abs ((double) chorus.getDelay (channel, voice).get()
                                                                - Chorus::getVoiceDelay (
                                                                    values, Chorus::getVoicePhase (values, phase, channel, voice), voice)));
                               });

        CHECK (worst < 0.01); // milliseconds
        CHECK (isFiniteAndBelow (out, 4.0f));

        // And as loud as one copy a side is there, set the same.
        auto single = values;
        single[Chorus::voices] = voicesFor (1);
        chorus.prepare (sampleRate, blockSize);
        const auto one = play (chorus, single, makeNoise (numSamples), blockSize);

        for (int channel = 0; channel < 2; ++channel)
            CHECK (std::abs (levelOf (out, channel) - levelOf (one, channel)) < 2.0);
    }

    // In blocks of 64, 480 and 512 samples (whole control steps) it plays the same samples.
    const auto noise = makeNoise (24000);
    Chorus chorus;
    chorus.prepare (usualRate, 512);
    const auto reference = play (chorus, values, noise, 480);

    for (const auto blockSize : { 64, 512 })
    {
        chorus.prepare (usualRate, 512);
        CHECK (areSame (play (chorus, values, noise, blockSize), reference));
    }

    // In blocks of 37, which the control steps don't divide, a slow sweep's tone is the same within a hair (each copy's delay
    // glides to where the LFO was as a step began, so the steps' places move it by a part of a sample).
    values[Chorus::delay] = 8.0f;
    values[Chorus::depth] = 0.6f;
    values[Chorus::rate] = 0.7f;
    const auto tone = makeSine (300.0, 48000);
    chorus.prepare (usualRate, 512);
    const auto even = play (chorus, values, tone, 480);
    chorus.prepare (usualRate, 512);
    const auto odd = play (chorus, values, tone, 37);
    auto furthest = 0.0f;

    for (int i = 0; i < 48000; ++i)
        furthest = std::max (furthest, std::abs (odd.getSample (0, i) - even.getSample (0, i)));

    CHECK (furthest < 0.01f);
}

//==============================================================================
// Safe Bass: the sound split at its frequency, the copies hearing only the band above it, 24 dB an octave down below it, and
// the band below going around them; the two bands, and what the runner mixes back in, adding up flat.
namespace
{
juce::AudioBuffer<float> playThroughRunner (const std::vector<float>& values, const std::vector<float>& sound,
                                            const std::function<void (int, std::vector<float>&)>& beforeBlock = {})
{
    return phazite::test::playThroughRunner (Chorus::info, std::make_unique<Chorus>(), values, sound, usualRate, usualBlock, beforeBlock);
}

// One voice a side, or three each sweeping its part of the sweep fast, the sides opposite, as Solid Bass Chorus has them; or
// eight, fed back and darkened.
std::vector<float> getEnsemble (int count)
{
    auto values = getDefaults();
    values[Chorus::voices] = voicesFor (count);
    values[Chorus::delay] = 5.0f;
    values[Chorus::rate] = 3.0f;
    values[Chorus::spread] = 0.4f;

    if (count > 3)
    {
        values[Chorus::feedback] = 0.6f;
        values[Chorus::highCut] = 6000.0f;
    }

    return values;
}
} // namespace

TEST_CASE ("The chorus's Safe Bass leaves a 40 Hz tone under 150 Hz as it was, with any voices: unmoved, the same on both sides")
{
    const auto tone = phazite::test::makeTone (40.0, 96000, usualRate);

    for (const auto count : { 1, 3, 8 })
    {
        CAPTURE (count);
        auto values = getEnsemble (count);

        // Without it, the copies alone change the tone: move it, or, many, line up on it and make it louder.
        values[Chorus::mix] = 1.0f;
        const auto swept = phazite::test::readTone (playThroughRunner (values, tone), 40.0, usualRate, 48000, 96000);
        CHECK ((std::abs (phazite::test::inDecibels (swept.left / 0.5)) > 1.0 || phazite::test::inDecibels (swept.unsteady / 0.5) > -30.0));

        // With it at 150 Hz, at any mix: as loud as it went in, within 0.2 dB, the same on both sides, and nothing of it
        // moving, each 40 dB under the tone.
        values[Chorus::safeBass] = 150.0f;

        for (const auto mix : { 1.0f, 0.5f })
        {
            CAPTURE (mix);
            values[Chorus::mix] = mix;
            const auto kept = phazite::test::readTone (playThroughRunner (values, tone), 40.0, usualRate, 48000, 96000);
            CHECK (std::abs (phazite::test::inDecibels (kept.left / 0.5)) < 0.2);
            CHECK (std::abs (phazite::test::inDecibels (kept.right / 0.5)) < 0.2);
            CHECK (phazite::test::inDecibels (kept.side / 0.5) < -40.0);
            CHECK (phazite::test::inDecibels (kept.unsteady / 0.5) < -40.0);
        }
    }
}

TEST_CASE ("The chorus's Safe Bass sends its copies a 75 Hz tone under 150 Hz 24 dB down, and an octave lower 48")
{
    // What of a tone reaches the copies: the difference between what the chorus plays with its copy standing still at two
    // delays half the tone's period apart (upside down from one to the other, the band around it the same in both), over 2.
    // A Linkwitz-Riley split's: 24 dB an octave down below its frequency; at least 10 dB further down than what is left once
    // a 12 dB low-pass is taken away, which falls only 6 dB an octave (2.8 dB down at 75 Hz).
    namespace dsp = devices::dsp;
    auto values = getDefaults();
    values[Chorus::depth] = 0.0f;
    values[Chorus::stereo] = 0.0f;
    values[Chorus::safeBass] = 150.0f;
    values[Chorus::mix] = 1.0f;

    for (const auto hertz : { 75.0, 37.5 })
    {
        CAPTURE (hertz);
        const auto tone = phazite::test::makeTone (hertz, 48000, usualRate);
        auto later = values;
        values[Chorus::delay] = 2.0f;
        later[Chorus::delay] = (float) (2.0 + 500.0 / hertz);
        auto difference = playThroughRunner (values, tone);
        const auto second = playThroughRunner (later, tone);

        for (int channel = 0; channel < 2; ++channel)
            difference.addFrom (channel, 0, second, channel, 0, difference.getNumSamples(), -1.0f);

        const auto part = phazite::test::readTone (difference, hertz, usualRate, 24000, 48000).left / 0.5 / 2.0;
        const auto high = dsp::getResponse (dsp::makeFilter (dsp::FilterShape::highPass, 150.0, usualRate, 0.70710678), hertz, usualRate);
        const auto low = dsp::getResponse (dsp::makeFilter (dsp::FilterShape::lowPass, 150.0, usualRate, 0.70710678), hertz, usualRate);
        CHECK (phazite::test::inDecibels (part) == doctest::Approx (phazite::test::inDecibels (std::abs (high * high))).epsilon (0.02));
        CHECK (phazite::test::inDecibels (part) < phazite::test::inDecibels (std::abs (1.0 - low)) - 10.0);
    }
}

TEST_CASE ("The chorus's Safe Bass adds back up flat: its copy at its shortest, it plays flat across Safe Bass at any mix")
{
    // One copy at the bottom of a deep, slow sweep, lagging no more than three samples over the 300 ms it plays: it leaves the
    // band above Safe Bass all but alone. Then tones from two octaves under it to two over come out as loud as they went in,
    // within 0.1 dB, at any mix: the two bands add back up flat, and what the runner mixes back in with them is in phase
    // with them.
    auto values = getDefaults();
    values[Chorus::delay] = 1.0f;
    values[Chorus::depth] = 1.0f;
    values[Chorus::rate] = 0.05f;
    values[Chorus::stereo] = 0.0f;
    values[Chorus::safeBass] = 150.0f;

    for (const auto mix : { 1.0f, 0.5f, 0.0f })
    {
        values[Chorus::mix] = mix;

        for (int step = 0; step <= 8; ++step)
        {
            const auto hertz = 37.5 * std::exp2 (step / 2.0);
            CAPTURE (mix);
            CAPTURE (hertz);
            const auto out = playThroughRunner (values, phazite::test::makeTone (hertz, 14400, usualRate));
            const auto parts = phazite::test::readTone (out, hertz, usualRate, 4800, 14400);
            CHECK (std::abs (phazite::test::inDecibels (parts.left / 0.5)) < 0.1);
            CHECK (std::abs (phazite::test::inDecibels (parts.right / 0.5)) < 0.1);
        }
    }
}

TEST_CASE ("The chorus's Safe Bass turns on, off and across its range without a click, with one voice or eight")
{
    // A low tone and a higher one, and Safe Bass turned on, across its range, off and on again while they play, by hand (its
    // runner gliding it over a block) and at once, from one block to the next: in the 200 ms after each change, no sample
    // jumps from the one before by much more than any does while Safe Bass stays where it is.
    std::vector<float> tones (240000);

    for (size_t i = 0; i < tones.size(); ++i)
        tones[i] = 0.25f * (float) (std::sin (twoPi * 60.0 * (double) i / usualRate) + std::sin (twoPi * 250.0 * (double) i / usualRate));

    constexpr int changes[] = { 50, 100, 150, 200 }; // blocks
    constexpr int settling = 20;                     // blocks after a change
    const auto changeAt = [] (int block, std::vector<float>& values)
    {
        for (const auto& [at, hertz] : { std::pair { 50, 150.0f }, { 100, 600.0f }, { 150, 20.0f }, { 200, 400.0f } })
            if (block == at)
                values[Chorus::safeBass] = hertz;
    };

    for (auto values : { getDefaults(), getEnsemble (8) })
    {
        CAPTURE (Chorus::getNumVoices (values));
        const auto byHand = playThroughRunner (values, tones, changeAt);
        Chorus chorus;
        chorus.prepare (usualRate, usualBlock);
        const auto atOnce = play (chorus, values, tones, usualBlock, [&] (int block) { changeAt (block + 1, values); });

        for (const auto* out : { &byHand, &atOnce })
        {
            auto steady = largestStep (*out, 10 * usualBlock, changes[0] * usualBlock);

            for (size_t k = 0; k < std::size (changes); ++k)
                steady = std::max (steady, largestStep (*out, (changes[k] + settling) * usualBlock,
                                                        k + 1 < std::size (changes) ? changes[k + 1] * usualBlock : out->getNumSamples()));

            CHECK (steady > 0.001f);

            for (const auto change : changes)
            {
                CAPTURE (change);
                CHECK (largestStep (*out, change * usualBlock, (change + settling) * usualBlock) < 2.0f * steady);
            }
        }
    }
}
