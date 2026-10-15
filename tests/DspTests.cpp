// SPDX-License-Identifier: AGPL-3.0-or-later
// The building blocks of Phazite's devices' DSP (src/devices/dsp/): filters, envelopes, oscillators, random numbers,
// delay lines.
#include "TestHelpers.h"
#include "devices/dsp/DelayLine.h"
#include "devices/dsp/Envelopes.h"
#include "devices/dsp/Filters.h"
#include "devices/dsp/Math.h"
#include "devices/dsp/Oscillators.h"
#include "devices/dsp/Random.h"

#include <cmath>
#include <vector>

namespace
{
namespace dsp = phazite::devices::dsp;
constexpr double testRate = 48000.0;
constexpr double twoPi = juce::MathConstants<double>::twoPi;

double toDecibels (double gain)
{
    return 20.0 * std::log10 (gain);
}

double responseDecibels (const dsp::FilterCoefficients& c, double frequency)
{
    return toDecibels (dsp::getMagnitude (c, frequency, testRate));
}

// A sine through a filter: its level once the filter has settled, relative to the sine's, in decibels.
double measureDecibels (const dsp::FilterCoefficients& c, double frequency)
{
    dsp::Filter filter;
    const auto settle = (int) testRate / 2, measure = (int) testRate;
    double input = 0.0, output = 0.0;

    for (int i = 0; i < settle + measure; ++i)
    {
        const auto x = (float) std::sin (twoPi * frequency * i / testRate);
        const auto y = filter.process (x, c);

        if (i >= settle)
        {
            input += (double) x * x;
            output += (double) y * y;
        }
    }

    return 10.0 * std::log10 (output / input);
}

// How many times a signal goes from below zero to zero or above.
template <typename Next>
int countRisingCrossings (int numSamples, Next next)
{
    int crossings = 0;
    auto previous = next();

    for (int i = 1; i < numSamples; ++i)
    {
        const auto value = next();

        if (previous < 0.0f && value >= 0.0f)
            ++crossings;

        previous = value;
    }

    return crossings;
}
} // namespace

TEST_CASE ("Filter shapes have the gains their names promise")
{
    const auto lowPass = dsp::makeFilter (dsp::FilterShape::lowPass, 1000.0, testRate, 0.70710678);
    CHECK (responseDecibels (lowPass, 1000.0) == doctest::Approx (-3.0103).epsilon (0.01));
    CHECK (responseDecibels (lowPass, 100.0) == doctest::Approx (0.0).epsilon (0.01));
    CHECK (responseDecibels (lowPass, 10000.0) < -35.0);

    const auto highPass = dsp::makeFilter (dsp::FilterShape::highPass, 1000.0, testRate, 0.70710678);
    CHECK (responseDecibels (highPass, 1000.0) == doctest::Approx (-3.0103).epsilon (0.01));
    CHECK (std::abs (responseDecibels (highPass, 12000.0)) < 0.1);
    CHECK (responseDecibels (highPass, 100.0) < -35.0);

    const auto bandPass = dsp::makeFilter (dsp::FilterShape::bandPass, 1000.0, testRate, 4.0);
    CHECK (std::abs (responseDecibels (bandPass, 1000.0)) < 0.01);
    CHECK (responseDecibels (bandPass, 4000.0) < -20.0);

    const auto bell = dsp::makeFilter (dsp::FilterShape::bell, 1000.0, testRate, 1.0, 6.0);
    CHECK (responseDecibels (bell, 1000.0) == doctest::Approx (6.0).epsilon (0.005));
    CHECK (std::abs (responseDecibels (bell, 40.0)) < 0.2);
    CHECK (std::abs (responseDecibels (bell, 18000.0)) < 0.3);

    const auto cut = dsp::makeFilter (dsp::FilterShape::bell, 1000.0, testRate, 1.0, -12.0);
    CHECK (responseDecibels (cut, 1000.0) == doctest::Approx (-12.0).epsilon (0.005));

    const auto lowShelf = dsp::makeFilter (dsp::FilterShape::lowShelf, 1000.0, testRate, 0.70710678, 6.0);
    CHECK (responseDecibels (lowShelf, 20.0) == doctest::Approx (6.0).epsilon (0.01));
    CHECK (responseDecibels (lowShelf, 1000.0) == doctest::Approx (3.0).epsilon (0.01)); // half way, in decibels
    CHECK (std::abs (responseDecibels (lowShelf, 18000.0)) < 0.1);

    const auto highShelf = dsp::makeFilter (dsp::FilterShape::highShelf, 1000.0, testRate, 0.70710678, -6.0);
    CHECK (std::abs (responseDecibels (highShelf, 20.0)) < 0.1);
    CHECK (responseDecibels (highShelf, 1000.0) == doctest::Approx (-3.0).epsilon (0.01));
    CHECK (responseDecibels (highShelf, 20000.0) == doctest::Approx (-6.0).epsilon (0.01));

    const auto notch = dsp::makeFilter (dsp::FilterShape::notch, 1000.0, testRate, 2.0);
    CHECK (responseDecibels (notch, 1000.0) < -60.0);
    CHECK (std::abs (responseDecibels (notch, 100.0)) < 0.1);

    const auto allPass = dsp::makeFilter (dsp::FilterShape::allPass, 1000.0, testRate, 0.70710678);

    for (const auto frequency : { 30.0, 1000.0, 5000.0, 20000.0 })
        CHECK (std::abs (responseDecibels (allPass, frequency)) < 0.001);

    // Frequencies at or past the Nyquist frequency are kept just below it.
    const auto high = dsp::makeFilter (dsp::FilterShape::lowPass, 30000.0, testRate, 0.70710678);
    CHECK (std::isfinite (high.a1));
    CHECK (std::isfinite (responseDecibels (high, 1000.0)));
}

TEST_CASE ("A filter's measured response matches the response it reports")
{
    const auto lowPass = dsp::makeFilter (dsp::FilterShape::lowPass, 1000.0, testRate, 0.70710678);
    const auto bell = dsp::makeFilter (dsp::FilterShape::bell, 2000.0, testRate, 2.0, 9.0);
    const auto shelf = dsp::makeFilter (dsp::FilterShape::highShelf, 3000.0, testRate, 0.70710678, -8.0);

    for (const auto frequency : { 250.0, 1000.0, 2000.0, 4000.0, 9000.0 })
    {
        CAPTURE (frequency);
        CHECK (measureDecibels (lowPass, frequency) == doctest::Approx (responseDecibels (lowPass, frequency)).epsilon (0.01));
        CHECK (measureDecibels (bell, frequency) == doctest::Approx (responseDecibels (bell, frequency)).epsilon (0.01));
        CHECK (measureDecibels (shelf, frequency) == doctest::Approx (responseDecibels (shelf, frequency)).epsilon (0.01));
    }
}

TEST_CASE ("Butterworth sections in a row give a flat slope, -3 dB at the cutoff")
{
    CHECK (dsp::getButterworthQ (2, 0) == doctest::Approx (0.70710678));
    CHECK (dsp::getButterworthQ (4, 0) == doctest::Approx (0.5411961));
    CHECK (dsp::getButterworthQ (4, 1) == doctest::Approx (1.3065630));

    for (const auto order : { 2, 4, 6, 8 })
    {
        CAPTURE (order);
        double atCutoff = 0.0, octaveUp = 0.0;

        for (int section = 0; section < order / 2; ++section)
        {
            const auto c = dsp::makeFilter (dsp::FilterShape::lowPass, 1000.0, testRate, dsp::getButterworthQ (order, section));
            atCutoff += responseDecibels (c, 1000.0);
            octaveUp += responseDecibels (c, 2000.0);
        }

        CHECK (atCutoff == doctest::Approx (-3.0103).epsilon (0.01));
        // An analog Butterworth filter is down 10 log10 (1 + 2^(2n)) an octave up; the digital one a little more.
        CHECK (octaveUp == doctest::Approx (-10.0 * std::log10 (1.0 + std::pow (2.0, 2.0 * order))).epsilon (0.03));
    }
}

TEST_CASE ("Random numbers repeat from the same seed and stay in range")
{
    dsp::Random a (42), b (42), c (43);
    bool differs = false;

    for (int i = 0; i < 1000; ++i)
    {
        const auto x = a.nextInt();
        CHECK (x == b.nextInt());
        differs = differs || x != c.nextInt();
    }

    CHECK (differs);

    double sum = 0.0;
    auto lowest = 1.0f, highest = -1.0f;

    for (int i = 0; i < 100000; ++i)
    {
        const auto x = a.nextBipolar();
        sum += x;
        lowest = std::min (lowest, x);
        highest = std::max (highest, x);
    }

    CHECK (lowest >= -1.0f);
    CHECK (highest < 1.0f);
    CHECK (std::abs (sum / 100000.0) < 0.01);

    dsp::Random zero (0); // a seed of 0 would give only zeros, so it is replaced
    CHECK (zero.nextInt() != 0u);
}

TEST_CASE ("An ADSR takes exactly its times, and moves on from where it is")
{
    dsp::Adsr adsr;
    adsr.prepare (testRate);
    adsr.setParameters (0.01f, 0.02f, 0.5f, 0.05f); // 480, 960 and 2400 samples
    CHECK_FALSE (adsr.isActive());

    adsr.noteOn();
    float level = 0.0f;

    for (int i = 0; i < 240; ++i)
        level = adsr.next();

    CHECK (level > 0.5f); // the attack curves up quickly, as analog ones do
    CHECK (level < 1.0f);

    for (int i = 240; i < 480; ++i)
        level = adsr.next();

    CHECK (level == doctest::Approx (1.0f));

    for (int i = 0; i < 960; ++i)
        level = adsr.next();

    CHECK (level == doctest::Approx (0.5f));

    for (int i = 0; i < 1000; ++i)
        level = adsr.next();

    CHECK (level == doctest::Approx (0.5f)); // sustaining

    adsr.noteOff();
    CHECK (adsr.isReleasing());

    for (int i = 0; i < 1200; ++i)
        level = adsr.next();

    CHECK (level > 0.0f);
    CHECK (level < 0.25f);

    // A new note during the release attacks from the level reached, without a jump.
    adsr.noteOn();
    CHECK (std::abs (adsr.next() - level) < 0.01f);

    adsr.noteOff();

    for (int i = 0; i < 2400; ++i)
        adsr.next();

    CHECK_FALSE (adsr.isActive());
    CHECK (adsr.getLevel() == doctest::Approx (0.0f));

    // No attack and no decay: straight to the sustain level; no release: silent at once.
    adsr.setParameters (0.0f, 0.0f, 0.7f, 0.0f);
    adsr.noteOn();
    CHECK (adsr.next() == doctest::Approx (0.7f));
    adsr.noteOff();
    CHECK_FALSE (adsr.isActive());
    CHECK (adsr.next() == doctest::Approx (0.0f));
}

TEST_CASE ("A level follower rises and falls with its time constants")
{
    dsp::LevelFollower follower;
    follower.prepare (testRate);
    follower.setTimes (0.01f, 0.1f);

    for (int i = 0; i < 480; ++i)
        follower.process (1.0f);

    CHECK (follower.getValue() == doctest::Approx (1.0 - std::exp (-1.0)).epsilon (0.01));

    follower.reset (1.0f);

    for (int i = 0; i < 4800; ++i)
        follower.process (0.0f);

    CHECK (follower.getValue() == doctest::Approx (std::exp (-1.0)).epsilon (0.01));
}

TEST_CASE ("Oscillators play at their frequency, their pulse at its width, and their sine is accurate")
{
    for (const auto waveform : { dsp::Waveform::sine, dsp::Waveform::triangle, dsp::Waveform::saw, dsp::Waveform::square })
    {
        dsp::Oscillator oscillator;
        oscillator.setFrequency (1000.0, testRate);
        auto peak = 0.0f;
        const auto crossings = countRisingCrossings ((int) testRate,
                                                     [&]
                                                     {
                                                         const auto value = oscillator.next (waveform);
                                                         peak = std::max (peak, std::abs (value));
                                                         return value;
                                                     });
        CAPTURE ((int) waveform);
        CHECK (std::abs (crossings - 1000) <= 1);
        CHECK (peak > 0.9f);
        CHECK (peak < 1.1f);
    }

    dsp::Oscillator pulse;
    pulse.setFrequency (100.0, testRate);
    int high = 0;

    for (int i = 0; i < (int) testRate; ++i)
        if (pulse.next (dsp::Waveform::square, 0.25f) > 0.0f)
            ++high;

    CHECK (high / testRate == doctest::Approx (0.25).epsilon (0.02));

    auto worst = 0.0;

    for (int i = 0; i <= 10000; ++i)
    {
        const auto phase = i / 10000.0;
        worst = std::max (worst, std::abs (dsp::sineOfPhase ((float) phase) - std::sin (twoPi * phase)));
    }

    CHECK (worst < 0.0002);
}

TEST_CASE ("An LFO's period follows its rate, and its random steps repeat from a seed")
{
    dsp::Lfo lfo;
    lfo.prepare (testRate);
    lfo.setRate (2.0);
    lfo.reset();

    // A rising saw falls back once a cycle: every 24,000 samples at 2 Hz, so twice in 1.25 seconds.
    int falls = 0;
    auto previous = lfo.next (dsp::LfoShape::sawUp);

    for (int i = 1; i < (int) (1.25 * testRate); ++i)
    {
        const auto value = lfo.next (dsp::LfoShape::sawUp);

        if (value < previous)
            ++falls;

        previous = value;
    }

    CHECK (falls == 2);

    dsp::Lfo a, b;

    for (auto* lfoToSet : { &a, &b })
    {
        lfoToSet->prepare (testRate);
        lfoToSet->setRate (100.0);
        lfoToSet->reset (0.0f, 7);
    }

    std::vector<float> values;

    for (int i = 0; i < 4800; ++i)
    {
        const auto value = a.next (dsp::LfoShape::random);
        CHECK (value == doctest::Approx (b.next (dsp::LfoShape::random)));
        values.push_back (value);
    }

    // Held for a whole cycle of 480 samples, then a new value.
    CHECK (values[100] == doctest::Approx (values[0]));
    CHECK (values[470] == doctest::Approx (values[0]));
    CHECK (std::abs (values[600] - values[0]) > 1.0e-6f);
}

TEST_CASE ("A delay line returns what was pushed, also between samples")
{
    dsp::DelayLine line;
    line.prepare (100);

    line.push (1.0f);
    CHECK (line.read (0) == doctest::Approx (1.0f));

    for (int i = 0; i < 10; ++i)
        line.push (0.0f);

    CHECK (line.read (10) == doctest::Approx (1.0f));
    CHECK (line.read (9) == doctest::Approx (0.0f));
    CHECK (line.read (9.5f) == doctest::Approx (0.5f));
    CHECK (line.read (9.25f) == doctest::Approx (0.25f));

    for (int i = 0; i < 90; ++i)
        line.push (0.0f);

    CHECK (line.read (100) == doctest::Approx (1.0f)); // the longest delay it was prepared for

    line.reset();
    CHECK (line.read (100) == doctest::Approx (0.0f));
}

TEST_CASE ("Decibels, saturation and note frequencies")
{
    CHECK (dsp::decibelsToGain (-6.0206f) == doctest::Approx (0.5f).epsilon (0.001));
    CHECK (dsp::decibelsToGain (-130.0f) == doctest::Approx (0.0f));
    CHECK (dsp::gainToDecibels (0.0f) == doctest::Approx (-120.0f));
    CHECK (dsp::gainToDecibels (2.0f) == doctest::Approx (6.0206f).epsilon (0.001));

    CHECK (dsp::softClip (3.0f) == doctest::Approx (1.0f));
    CHECK (dsp::softClip (10.0f) == doctest::Approx (1.0f));
    CHECK (dsp::softClip (-10.0f) == doctest::Approx (-1.0f));
    CHECK (dsp::softClip (0.1f) == doctest::Approx (std::tanh (0.1f)).epsilon (0.001));

    // Rising throughout (it flattens out toward 3, where it reaches 1 with a slope of 0).
    for (auto x = -2.5f; x < 2.5f; x += 0.01f)
        CHECK (dsp::softClip (x + 0.01f) > dsp::softClip (x));

    CHECK (dsp::noteToFrequency (69.0) == doctest::Approx (440.0));
    CHECK (dsp::noteToFrequency (81.0) == doctest::Approx (880.0));
    CHECK (dsp::noteToFrequency (60.0) == doctest::Approx (261.6256).epsilon (0.0001));
}
