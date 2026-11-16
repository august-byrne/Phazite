// SPDX-License-Identifier: AGPL-3.0-or-later
// The building blocks of Phazite's devices' DSP (src/devices/dsp/): filters, comb filters, the filter types every Phazite
// filter offers, envelopes, oscillators, random numbers, delay lines.
#include "TestHelpers.h"
#include "devices/dsp/AlignedShaper.h"
#include "devices/dsp/CombFilter.h"
#include "devices/dsp/DcBlocker.h"
#include "devices/dsp/DelayLine.h"
#include "devices/dsp/Envelopes.h"
#include "devices/dsp/Filters.h"
#include "devices/dsp/Math.h"
#include "devices/dsp/MultimodeFilter.h"
#include "devices/dsp/Noise.h"
#include "devices/dsp/Oscillators.h"
#include "devices/dsp/Random.h"
#include "devices/dynamics/TruePeak.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>
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

dsp::CombFilter makeCombFilter (const dsp::CombCoefficients& coefficients)
{
    dsp::CombFilter comb;
    comb.prepare (dsp::getLongestCombDelay (20.0, testRate));
    comb.setTarget (coefficients, 16); // the first after preparing: at once
    return comb;
}

// A sine through a comb: its level once the comb has settled, relative to the sine's, as a gain.
double measureCombGain (const dsp::CombCoefficients& coefficients, double frequency)
{
    auto comb = makeCombFilter (coefficients);
    const auto settle = (int) testRate, measure = (int) testRate / 2;
    double input = 0.0, output = 0.0;

    for (int i = 0; i < settle + measure; ++i)
    {
        const auto x = (float) std::sin (twoPi * frequency * i / testRate);
        const auto y = comb.process (x);

        if (i >= settle)
        {
            input += (double) x * x;
            output += (double) y * y;
        }
    }

    return std::sqrt (output / input);
}

// The RMS level of white noise through a comb, once it has settled.
double measureNoiseThrough (const dsp::CombCoefficients& coefficients)
{
    auto comb = makeCombFilter (coefficients);
    dsp::Random noise (0x5eedu);
    auto sum = 0.0;

    for (int i = 0; i < 3 * (int) testRate; ++i)
    {
        const auto y = comb.process (noise.nextBipolar());

        if (i >= (int) testRate)
            sum += (double) y * y;
    }

    return std::sqrt (sum / (2.0 * testRate));
}

// One of the filter types at a cutoff, with a 12 dB section's Q as the Filter device's.
dsp::MultimodeCoefficients makeType (dsp::FilterMode mode, double cutoff, float resonance, bool steep = false)
{
    return dsp::makeMultimode ({ .mode = mode,
                                 .frequency = cutoff,
                                 .resonance = resonance,
                                 .steep = steep,
                                 .q = 0.70710678 * std::pow (2.0, (double) resonance * 4.5) },
                               testRate);
}

dsp::MultimodeFilter makeMultimodeFilter (const dsp::MultimodeCoefficients& coefficients)
{
    dsp::MultimodeFilter filter;
    filter.prepare (dsp::getLongestCombDelay (20.0, testRate));
    filter.set (coefficients, 0);
    return filter;
}

// A sine through one of the filter types: its level once settled, relative to the sine's, as a gain.
double measureMultimodeGain (const dsp::MultimodeCoefficients& coefficients, double frequency)
{
    auto filter = makeMultimodeFilter (coefficients);
    const auto settle = (int) testRate, measure = (int) testRate / 2;
    double input = 0.0, output = 0.0;

    for (int i = 0; i < settle + measure; ++i)
    {
        const auto x = (float) std::sin (twoPi * frequency * i / testRate);
        const auto y = filter.process (x);

        if (i >= settle)
        {
            input += (double) x * x;
            output += (double) y * y;
        }
    }

    return std::sqrt (output / input);
}

// How strong a frequency is in a stretch of sound, as an amplitude: its DFT there.
double measureAmplitudeAt (const std::vector<float>& sound, size_t from, size_t to, double frequency)
{
    auto re = 0.0, im = 0.0;

    for (auto i = from; i < to; ++i)
    {
        re += sound[i] * std::cos (twoPi * frequency * (double) i / testRate);
        im += sound[i] * std::sin (twoPi * frequency * (double) i / testRate);
    }

    return 2.0 * std::sqrt (re * re + im * im) / (double) (to - from);
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

TEST_CASE ("An ADSR's stages bend either way along their curves, and still take exactly their times")
{
    // Halfway through a 480-sample attack, for a curve: where the envelope is, and where its shape says it is.
    const auto halfwayUp = [] (float curve)
    {
        dsp::Adsr adsr;
        adsr.prepare (testRate);
        adsr.setParameters (0.01f, 0.0f, 1.0f, 0.0f);
        adsr.setCurves (curve, 0.0f, 0.0f);
        adsr.noteOn();
        float level = 0.0f;

        for (int i = 0; i < 240; ++i)
            level = adsr.next();

        CHECK (level == doctest::Approx (dsp::Adsr::getShape (curve, 0.5f)).epsilon (0.01));

        for (int i = 240; i < 480; ++i)
            level = adsr.next();

        CHECK (level == doctest::Approx (1.0f)); // on its target as its time is up, whatever its curve
        return dsp::Adsr::getShape (curve, 0.5f);
    };

    CHECK (halfwayUp (0.0f) == doctest::Approx (0.5f)); // straight
    CHECK (halfwayUp (-1.0f) > 0.95f);                  // fast at first
    CHECK (halfwayUp (1.0f) < 0.05f);                   // slow at first
    // An analog attack, as the synths played before: aiming 30 % past its target, halfway it has gone 1.3 (1 - sqrt (0.3 / 1.3)).
    CHECK (halfwayUp (dsp::Adsr::analogAttackCurve) == doctest::Approx (1.3f * (1.0f - std::sqrt (0.3f / 1.3f))).epsilon (0.001));

    // The release from where it is, along its curve, never below silence.
    dsp::Adsr adsr;
    adsr.prepare (testRate);
    adsr.setParameters (0.0f, 0.0f, 0.8f, 0.01f);
    adsr.setCurves (0.0f, 0.0f, 0.5f);
    adsr.noteOn();
    adsr.next();
    adsr.noteOff();
    auto lowest = 1.0f;

    for (int i = 0; i < 480; ++i)
        lowest = std::min (lowest, adsr.next());

    CHECK (lowest >= 0.0f);
    CHECK_FALSE (adsr.isActive());
}

TEST_CASE ("An ADSR follows a new sustain level over the same time at any rate it runs at")
{
    // Sustaining at full, its sustain turned down to half: how long it takes to go 63 % of the way, at the rates a synth runs
    // its envelopes at, a control rate among them. It moved a fixed part of the way each sample, so twice as fast at 96 kHz.
    const auto secondsToFollow = [] (double rate)
    {
        dsp::Adsr adsr;
        adsr.prepare (rate);
        adsr.setParameters (0.0f, 0.0f, 1.0f, 0.1f);
        adsr.noteOn();
        adsr.next();
        adsr.setParameters (0.0f, 0.0f, 0.5f, 0.1f);
        auto samples = 0;

        while (adsr.next() > 1.0f - 0.632f * 0.5f && samples < 1000000)
            ++samples;

        return (samples + 1) / rate;
    };

    const auto at44 = secondsToFollow (44100.0);
    CHECK (at44 == doctest::Approx (0.0113).epsilon (0.02)); // as it always did at 44.1 kHz

    for (const auto rate : { 48000.0, 96000.0, 192000.0, 44100.0 / 16.0 })
    {
        CAPTURE (rate);
        CHECK (secondsToFollow (rate) == doctest::Approx (at44).epsilon (0.04)); // within a step of the control rate
    }
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
        CHECK (peak < 1.2f); // a band-limited wave overshoots each of its jumps by 9 % of it (Gibbs), a square most
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
    CHECK (line.read (10.0f) == 1.0f); // a whole number of samples: that sample, exactly

    // Between samples, by 4-point Lagrange interpolation: a lone sample's neighbours read as its cubic's, and a line, which a
    // cubic follows exactly, reads as itself.
    CHECK (line.read (9.5f) == doctest::Approx (0.5625f));
    CHECK (line.read (10.5f) == doctest::Approx (0.5625f));

    for (int i = 0; i < 90; ++i)
        line.push (0.0f);

    CHECK (line.read (100) == doctest::Approx (1.0f)); // the longest delay it was prepared for

    dsp::DelayLine ramp;
    ramp.prepare (16);

    for (int i = 0; i < 16; ++i)
        ramp.push ((float) i); // the one pushed d samples ago is 15 - d

    for (const auto delay : { 1.25f, 4.5f, 7.75f, 12.1f })
        CHECK (ramp.read (delay) == doctest::Approx (15.0f - delay).epsilon (1.0e-5));

    CHECK (ramp.read (0.5f) == doctest::Approx (14.5f)); // under a sample, a straight line

    line.reset();
    CHECK (line.read (100) == doctest::Approx (0.0f));
}

TEST_CASE ("A comb's peaks fall on its frequency's multiples, Comb -'s on its odd ones, with notches between, as it reports")
{
    // At half resonance its peaks rise by 4.25 dB.
    const auto plus = dsp::makeComb (false, 1000.0, testRate, 0.5f);
    const auto peak = std::pow (0.02, -0.5 * 0.25);

    for (const auto frequency : { 1000.0, 2000.0, 3000.0 })
    {
        INFO (frequency << " Hz");
        CHECK (dsp::getMagnitude (plus, frequency, testRate) == doctest::Approx (peak).epsilon (0.001));
        CHECK (measureCombGain (plus, frequency) == doctest::Approx (peak).epsilon (0.01));
    }

    for (const auto frequency : { 500.0, 1500.0, 2500.0 })
    {
        INFO (frequency << " Hz");
        CHECK (dsp::getMagnitude (plus, frequency, testRate) < 1.0e-6);
        CHECK (measureCombGain (plus, frequency) < 0.001);
    }

    // Comb -: at the same pitch, its even multiples gone, and anything steady with them.
    const auto minus = dsp::makeComb (true, 1000.0, testRate, 0.5f);

    for (const auto frequency : { 1000.0, 3000.0 })
        CHECK (measureCombGain (minus, frequency) == doctest::Approx (peak).epsilon (0.01));

    for (const auto frequency : { 2000.0, 4000.0 })
        CHECK (measureCombGain (minus, frequency) < 0.001);

    CHECK (dsp::getMagnitude (minus, 0.0, testRate) < 1.0e-6);

    // With its delay between samples (700 Hz: 68.6 samples), it sounds as it reports too.
    const auto between = dsp::makeComb (false, 700.0, testRate, 0.7f);

    for (const auto frequency : { 700.0, 2100.0 })
        CHECK (measureCombGain (between, frequency) == doctest::Approx (dsp::getMagnitude (between, frequency, testRate)).epsilon (0.02));

    for (const auto frequency : { 1050.0, 2450.0 })
        CHECK (measureCombGain (between, frequency) < 0.01);
}

TEST_CASE ("Resonance sharpens a comb's peaks into a ring as long as it reports, rising as much as the rest falls")
{
    const auto none = dsp::makeComb (false, 1000.0, testRate, 0.0f), most = dsp::makeComb (false, 1000.0, testRate, 1.0f);

    // The peaks rise from 0 to 8.5 dB...
    CHECK (dsp::getMagnitude (none, 1000.0, testRate) == doctest::Approx (1.0).epsilon (0.001));
    CHECK (toDecibels (dsp::getMagnitude (most, 1000.0, testRate)) == doctest::Approx (8.49).epsilon (0.01));

    // ... and narrow: a quarter of the way to the next one, little is left.
    CHECK (dsp::getMagnitude (none, 1250.0, testRate) == doctest::Approx (std::sqrt (0.5)).epsilon (0.001));
    CHECK (dsp::getMagnitude (most, 1250.0, testRate) < 0.05);

    // White noise, spread over peaks and notches alike, falls by as much as the peaks rise.
    CHECK (toDecibels (measureNoiseThrough (most) / measureNoiseThrough (none)) == doctest::Approx (-8.49).epsilon (0.1));

    // At its most, a comb at 440 Hz rings for about 0.8 s; at half, losing 1.3 dB a time round, about 0.1 s.
    const auto ringing = dsp::makeComb (false, 440.0, testRate, 1.0f);
    CHECK (dsp::getRingSeconds (ringing, testRate) == doctest::Approx (0.78).epsilon (0.02));
    CHECK (dsp::getRingSeconds (dsp::makeComb (false, 440.0, testRate, 0.5f), testRate) == doctest::Approx (0.105).epsilon (0.02));

    // A click through it: the time until it has fallen by 60 dB, from the level of its first round.
    auto comb = makeCombFilter (ringing);
    const auto period = (int) std::lround (testRate / 440.0);
    std::vector<double> rounds; // each period's RMS level

    for (int n = 0; n < 2 * (int) testRate;)
    {
        auto sum = 0.0;

        for (int k = 0; k < period; ++k, ++n)
        {
            const auto y = comb.process (n == 0 ? 1.0f : 0.0f);
            sum += (double) y * y;
        }

        rounds.push_back (std::sqrt (sum / period));
    }

    const auto first = rounds[1]; // the first round back from the delay
    auto fallen = rounds.size();

    for (size_t r = 1; r < rounds.size(); ++r)
    {
        if (rounds[r] < first * 0.001)
        {
            fallen = r;
            break;
        }
    }

    CHECK ((double) fallen * period / testRate == doctest::Approx (dsp::getRingSeconds (ringing, testRate)).epsilon (0.15));
}

TEST_CASE ("A comb glides to a new frequency rather than jumping, so that it doesn't click, and a reset silences it")
{
    // A 50 Hz sine through a comb at 100 Hz falls in a notch; at 50 Hz, on a peak. Jumping from one to the other at the
    // sine's crest would jump by its whole size; gliding over 16 samples, it rises within a few.
    auto comb = makeCombFilter (dsp::makeComb (false, 100.0, testRate, 0.0f));
    const auto crest = 24240; // the sine at its top
    std::vector<float> out;

    for (int i = 0; i < 48000; ++i)
    {
        if (i == crest)
            comb.setTarget (dsp::makeComb (false, 50.0, testRate, 0.0f), 16);

        out.push_back (comb.process (0.5f * (float) std::sin (twoPi * 50.0 * i / testRate)));
    }

    CHECK (std::abs (out[(size_t) crest - 1]) < 0.01f);
    CHECK (out[(size_t) crest + 960] > 0.45f); // a cycle later, at the crest again
    auto largestStep = 0.0f;

    for (size_t i = (size_t) crest - 100; i < (size_t) crest + 100; ++i)
        largestStep = std::max (largestStep, std::abs (out[i] - out[i - 1]));

    CHECK (largestStep < 0.1f);

    comb.reset();
    auto left = 0.0f;

    for (int i = 0; i < 4800; ++i)
        left = std::max (left, std::abs (comb.process (0.0f)));

    CHECK (left == doctest::Approx (0.0f));
}

TEST_CASE ("A comb stays steady at its most resonance, swept up and down the whole range on loud noise")
{
    for (const auto negative : { false, true })
    {
        INFO ((negative ? "Comb -" : "Comb +"));
        dsp::CombFilter comb;
        comb.prepare (dsp::getLongestCombDelay (20.0, testRate));
        dsp::Random noise (0xc0b5u);
        const auto length = 4 * (int) testRate;
        auto peak = 0.0f;

        for (int start = 0; start < length; start += 16)
        {
            // From 20 Hz to 20 kHz and back, the same number of octaves each step.
            const auto along = (double) start / length;
            const auto octaves = 9.97 * (along < 0.5 ? 2.0 * along : 2.0 - 2.0 * along);
            comb.setTarget (dsp::makeComb (negative, 20.0 * std::pow (2.0, octaves), testRate, 1.0f), 16);

            for (int i = 0; i < 16; ++i)
                peak = std::max (peak, std::abs (comb.process (noise.nextBipolar())));
        }

        CHECK (std::isfinite (peak));
        CHECK (peak < 8.0f);
    }
}

TEST_CASE ("Each of Phazite's filter types sounds as it reports")
{
    using dsp::FilterMode;

    struct Type
    {
        FilterMode mode;
        bool steep;
        float resonance;
    };

    for (const auto& type :
         { Type { FilterMode::lowPass, false, 0.3f }, Type { FilterMode::lowPass, true, 0.3f }, Type { FilterMode::highPass, true, 0.3f },
           Type { FilterMode::bandPass, false, 0.5f }, Type { FilterMode::bandPass, true, 0.5f }, Type { FilterMode::notch, true, 0.2f },
           Type { FilterMode::combPlus, false, 0.5f }, Type { FilterMode::vowel, false, 0.5f }, Type { FilterMode::phaser, false, 0.0f },
           Type { FilterMode::phaser, false, 0.8f } })
    {
        const auto coefficients = makeType (type.mode, 1000.0, type.resonance, type.steep);

        for (const auto frequency : { 250.0, 700.0, 1000.0, 1700.0, 4000.0 })
        {
            INFO ("type " << (int) type.mode << (type.steep ? " at 24 dB" : "") << ", " << frequency << " Hz");
            const auto reported = dsp::getMagnitude (coefficients, frequency, testRate);
            CHECK (std::abs (measureMultimodeGain (coefficients, frequency) - reported) < 0.01 + 0.03 * reported);
        }
    }
}

TEST_CASE ("The vowel filter's formants are a sung vowel's, which the cutoff picks from U through O, A and E to I")
{
    const auto vowel = [] (double cutoff, float resonance) { return makeType (dsp::FilterMode::vowel, cutoff, resonance); };
    const auto gain = [] (const dsp::MultimodeCoefficients& coefficients, double frequency)
    { return dsp::getMagnitude (coefficients, frequency, testRate); };

    // A, the cutoff at its second formant: peaks at 650, 1080 and 2650 Hz, well above what lies between them, the first
    // the loudest.
    const auto a = vowel (1080.0, 0.5f);
    CHECK (gain (a, 650.0) > 2.5 * gain (a, 850.0));
    CHECK (gain (a, 1080.0) > 3.0 * gain (a, 1800.0));
    CHECK (gain (a, 2650.0) > 3.0 * gain (a, 1800.0));
    CHECK (gain (a, 650.0) > gain (a, 1080.0));

    // U at the bottom and I at the top, with first formants of their own (350 and 290 Hz).
    CHECK (gain (vowel (600.0, 0.5f), 350.0) > 3.0 * gain (vowel (600.0, 0.5f), 470.0));
    CHECK (gain (vowel (1870.0, 0.5f), 290.0) > 3.0 * gain (vowel (1870.0, 0.5f), 450.0));

    // Between two vowels the second formant is the cutoff, and beyond I the formants move with it.
    for (const auto cutoff : { 1300.0, 1800.0, 3740.0 })
    {
        INFO (cutoff << " Hz");
        const auto between = vowel (cutoff, 0.5f);
        CHECK (gain (between, cutoff) > 1.5 * gain (between, cutoff * 1.1));
        CHECK (gain (between, cutoff) > 1.5 * gain (between, cutoff / 1.1));
    }

    // Resonance narrows the formants.
    const auto wide = vowel (1080.0, 0.0f), narrow = vowel (1080.0, 1.0f);
    CHECK (gain (narrow, 700.0) / gain (narrow, 650.0) < 0.5 * gain (wide, 700.0) / gain (wide, 650.0));
}

TEST_CASE ("The phaser's notches sit at a quarter of the cutoff, the cutoff and nearly four times it, and feedback raises its peaks")
{
    static constexpr auto pi = juce::MathConstants<double>::pi;

    // Where the stages' phase turns against what goes in by an odd number of half turns: tan (15, 45 and 75 degrees)
    // times the cutoff, as the bilinear transform warps it; the peaks between, at whole turns.
    const auto at = [] (double ratio, double cutoff) { return testRate / pi * std::atan (ratio * std::tan (pi * cutoff / testRate)); };

    for (const auto cutoff : { 1000.0, 2000.0 })
    {
        INFO (cutoff << " Hz");
        const auto plain = makeType (dsp::FilterMode::phaser, cutoff, 0.0f);

        for (const auto degrees : { 15.0, 45.0, 75.0 })
            CHECK (dsp::getMagnitude (plain, at (std::tan (degrees * pi / 180.0), cutoff), testRate) < 0.01);

        // At the most feedback the peaks rise by 3.4 dB, as much at any cutoff: the loop is solved at once.
        const auto peak = at (std::tan (pi / 6.0), cutoff);
        CHECK (dsp::getMagnitude (plain, peak, testRate) == doctest::Approx (1.0).epsilon (0.01));
        CHECK (toDecibels (dsp::getMagnitude (makeType (dsp::FilterMode::phaser, cutoff, 1.0f), peak, testRate))
               == doctest::Approx (3.4).epsilon (0.05));
    }
}

TEST_CASE ("Sample and hold keeps the sound at twice the cutoff's rate, folding back what lies above it")
{
    const auto through = [] (float resonance, double cutoff, const auto& input)
    {
        auto hold = makeMultimodeFilter (makeType (dsp::FilterMode::sampleAndHold, cutoff, resonance));
        std::vector<float> out;

        for (int i = 0; i < 9600; ++i)
            out.push_back (hold.process (input (i)));

        return out;
    };

    const auto sine = [] (double frequency)
    { return [frequency] (int i) { return 0.5f * (float) std::sin (twoPi * frequency * i / testRate); }; };

    // A cutoff of 1 kHz holds each sample for 24 at 48 kHz: 2.7 kHz, above it, folds back to 700 Hz, and 200 Hz comes
    // through nearly whole.
    const auto folded = through (0.0f, 1000.0, sine (2700.0));
    auto changes = 0;

    for (size_t i = 1; i < 4800; ++i)
        changes += std::abs (folded[i] - folded[i - 1]) > 1.0e-6f ? 1 : 0;

    CHECK (changes >= 190);
    CHECK (changes <= 200);
    CHECK (measureAmplitudeAt (folded, 4800, 9600, 700.0) > 0.1);
    CHECK (measureAmplitudeAt (through (0.0f, 1000.0, sine (200.0)), 4800, 9600, 200.0) > 0.45);

    // Resonance rings at the cutoff: a tone near it louder, what is steady at its level.
    const auto plainTone = measureAmplitudeAt (through (0.0f, 1000.0, sine (950.0)), 4800, 9600, 950.0);
    CHECK (measureAmplitudeAt (through (1.0f, 1000.0, sine (950.0)), 4800, 9600, 950.0) > 2.0 * plainTone);
    CHECK (through (1.0f, 1000.0, [] (int) { return 0.5f; }).back() == doctest::Approx (0.5f).epsilon (0.001));

    // At half the sample rate or above, with no resonance, it changes nothing.
    const auto open = through (0.0f, 24000.0, sine (5000.0));
    CHECK (open[100] == doctest::Approx (0.5f * (float) std::sin (twoPi * 5000.0 * 100 / testRate)));
}

TEST_CASE ("Phazite's combs, vowel, phaser and sample and hold stay steady at their most resonance, swept on loud noise")
{
    using dsp::FilterMode;

    for (const auto mode :
         { FilterMode::combPlus, FilterMode::combMinus, FilterMode::vowel, FilterMode::phaser, FilterMode::sampleAndHold })
    {
        INFO ("type " << (int) mode);
        dsp::MultimodeFilter filter;
        filter.prepare (dsp::getLongestCombDelay (20.0, testRate));
        dsp::Random noise (0x5a5au);
        const auto length = 2 * (int) testRate;
        auto peak = 0.0f;

        for (int start = 0; start < length; start += 16)
        {
            // From 20 Hz to 20 kHz and back, the same number of octaves each step.
            const auto along = (double) start / length;
            filter.set (makeType (mode, 20.0 * std::pow (2.0, 9.97 * (along < 0.5 ? 2.0 * along : 2.0 - 2.0 * along)), 1.0f), 16);

            for (int i = 0; i < 16; ++i)
                peak = std::max (peak, std::abs (filter.process (noise.nextBipolar())));
        }

        CHECK (std::isfinite (peak));
        CHECK (peak < 8.0f);
    }
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

TEST_CASE ("A whole number of samples made up from any delay leaves the all-pass a part it is stable at, and delays by it")
{
    // Made up to the next whole sample less a thousandth, a delay a hair over a whole number left a part a hair under 0,
    // where the all-pass's coefficient is over 1 and it runs away. Made up by half a sample to one and a half, every delay
    // leaves the all-pass a part it plays steadily, its delay at 0 Hz that part.
    for (auto latency = 5.0f; latency < 7.0f; latency += 0.0125f)
    {
        CAPTURE (latency);
        const auto whole = dsp::AlignedShaper::getWholeSamples (latency);
        const auto part = (float) whole - latency;
        CHECK (part >= dsp::FractionalDelay::shortest - 1.0e-4f);
        CHECK (part < dsp::FractionalDelay::longest + 1.0e-4f);

        dsp::FractionalDelay delay;
        delay.setDelay (std::clamp (part, dsp::FractionalDelay::shortest, dsp::FractionalDelay::longest - 1.0e-6f));
        auto weight = 0.0, moment = 0.0, last = 0.0;

        for (int n = 0; n < 256; ++n)
        {
            const auto y = (double) delay.process (n == 0 ? 1.0f : 0.0f);
            weight += y;
            moment += n * y;
            last = std::abs (y);
        }

        CHECK (weight == doctest::Approx (1.0).epsilon (1.0e-4));
        CHECK (moment / weight == doctest::Approx ((double) part).epsilon (1.0e-3));
        CHECK (last < 1.0e-9);
    }

    // The shaper's own: 5.137 samples, made up to 6.
    CHECK (dsp::AlignedShaper::getLatencySamples() == 6);
}

TEST_CASE ("The synths' noise is as it always was at 48 kHz, where the presets are leveled, and alike at the other rates")
{
    // The reviewer's finding 5 (2026-11-15), and the second review's finding 4 (2026-11-15): the reference is 48 kHz.
    // At 48 kHz the random numbers themselves. At 44.1, 96 and 192 kHz: as loud in each part of the band below 10 kHz
    // (from its spectrum), nothing above 30 kHz (where white noise at 96 kHz would hold more than half its power), as loud
    // in all, and as high the peaks of what lies below 10 kHz (rebuilt between its samples, four to each, from its
    // spectrum: as a converter rebuilds it, which the samples at 48 kHz show too little of, there so close to the band's
    // top). White noise at the device's own rate, turned up for the band heard, fails the second.
    dsp::Noise noise;
    dsp::Random random (0x1234u);
    noise.prepare (48000.0);
    noise.setSeed (0x1234u);
    auto same = true;

    for (int i = 0; i < 10000; ++i)
        same = same && juce::exactlyEqual (noise.next(), random.nextBipolar());

    CHECK (same);

    struct Measured
    {
        double total = 0.0, heard = 0.0, above = 0.0, heardPeak = 0.0;
    };

    const auto measure = [] (double rate)
    {
        dsp::Noise made;
        made.prepare (rate);
        made.setSeed (0x1234u);
        const auto order = rate > 100000.0 ? 18 : (rate > 50000.0 ? 17 : 16); // about a second and a half
        const auto size = 1 << order;
        std::vector<juce::dsp::Complex<float>> samples ((size_t) size), spectrum ((size_t) size);
        Measured measured;

        for (auto& sample : samples)
        {
            sample = made.next();
            measured.total += (double) sample.real() * sample.real();
        }

        measured.total /= size;
        juce::dsp::FFT (order).perform (samples.data(), spectrum.data(), false);

        // Below 10 kHz, alone, four times as many samples between, from its spectrum.
        const auto rebuiltOrder = order + 2;
        const auto rebuiltSize = 1 << rebuiltOrder;
        std::vector<juce::dsp::Complex<float>> kept ((size_t) rebuiltSize), rebuilt ((size_t) rebuiltSize);

        for (int bin = 1; bin < size / 2; ++bin)
        {
            const auto hertz = bin * rate / size;
            const auto power = 2.0 * std::norm (spectrum[(size_t) bin]) / ((double) size * size);

            if (hertz < 10000.0)
            {
                measured.heard += power;
                kept[(size_t) bin] = spectrum[(size_t) bin];
                kept[(size_t) (rebuiltSize - bin)] = std::conj (spectrum[(size_t) bin]);
            }
            else if (hertz > 30000.0)
            {
                measured.above += power;
            }
        }

        juce::dsp::FFT (rebuiltOrder).perform (kept.data(), rebuilt.data(), true);
        auto peak = 0.0, squares = 0.0;

        for (const auto& sample : rebuilt)
        {
            peak = std::max (peak, (double) std::abs (sample.real()));
            squares += (double) sample.real() * sample.real();
        }

        // As a part of its level, times its level from the spectrum: whatever the transforms scale by.
        measured.heardPeak = peak / std::sqrt (squares / rebuiltSize) * std::sqrt (measured.heard);
        return measured;
    };

    const auto at48 = measure (48000.0);

    for (const auto rate : { 44100.0, 96000.0, 192000.0 })
    {
        const auto at = measure (rate);
        const auto heard = 10.0 * std::log10 (at.heard / at48.heard), total = 10.0 * std::log10 (at.total / at48.total);
        const auto above = 10.0 * std::log10 (std::max (at.above, 1.0e-30) / at.total);
        const auto peak = 20.0 * std::log10 (at.heardPeak / at48.heardPeak);
        INFO (rate << " Hz: " << heard << " dB below 10 kHz, " << total << " dB in all, " << above << " dB above 30 kHz, peak " << peak
                   << " dB, against 48 kHz");
        CHECK (std::abs (heard) < 0.2);
        CHECK (std::abs (total) < 0.5);
        CHECK (std::abs (peak) < 1.0);

        if (rate > 60000.0)
            CHECK (above < -60.0);
    }
}

TEST_CASE (
    "An instrument's DC blocker takes an offset away, keeps 20 Hz, leaves nothing ringing after Stop, and comes and goes without a step")
{
    // devices/dsp/DcBlocker.h, on the synth's and the wavetable synth's sums. Stopping just after notes begin (PlaybackTests)
    // left its ring, a third of a thousandth, sounding for a tenth of a second after the notes had faded out.
    dsp::DcBlocker blocker;
    blocker.prepare (testRate);

    // A steady offset: gone after a second.
    auto last = 0.0f;

    for (int i = 0; i < (int) testRate; ++i)
        last = blocker.process (0.5f);

    CHECK (std::abs (last) < 1.0e-3f);

    // 20 Hz: its level within 0.02 dB, once settled.
    blocker.reset();
    auto peak = 0.0f;

    for (int i = 0; i < 4 * (int) testRate; ++i)
    {
        const auto y = blocker.process ((float) std::sin (twoPi * 20.0 * i / testRate));

        if (i > 3 * (int) testRate)
            peak = std::max (peak, std::abs (y));
    }

    CHECK (std::abs (toDecibels (peak)) < 0.02);

    // A note with an offset under it, then silence: its ring, after 20 ms of the silence, played as it is and faded out.
    const auto ringAfter = [] (bool quiet)
    {
        dsp::DcBlocker each;
        each.prepare (testRate);
        std::vector<float> samples ((size_t) testRate / 2);

        for (size_t i = 0; i < samples.size(); ++i)
            samples[i] = 0.1f + 0.5f * (float) std::sin (twoPi * 262.0 * (double) i / testRate);

        each.process (samples.data(), (int) samples.size(), true, false);
        std::vector<float> silence ((size_t) (0.04 * testRate));
        each.process (silence.data(), (int) silence.size(), true, quiet);

        auto left = 0.0f;

        for (auto i = (size_t) (0.02 * testRate); i < silence.size(); ++i)
            left = std::max (left, std::abs (silence[i]));

        return left;
    };

    const auto ringing = ringAfter (false); // what it would ring with: the offset it took away, falling
    CHECK (ringing > 0.01f);
    CHECK (ringAfter (true) < 0.01f * ringing);

    // The second review's finding 1 (2026-11-15): the instruments play it only where an offset can arise. Not wanted, it
    // leaves the samples as they are, bit for bit. Taken on in the middle of a sound with an offset, it starts at rest and
    // fades in, and let go, it fades out, so that neither steps: no sample moves from the one before by more than the sound
    // itself does, and a little more for the offset coming back over the fade (the offset over its 240 samples, and the phase the filter turns). Faded out,
    // the samples are as they were again, bit for bit.
    dsp::DcBlocker switched;
    switched.prepare (testRate);
    const auto length = (int) testRate;
    std::vector<float> sound ((size_t) length);

    for (size_t i = 0; i < sound.size(); ++i)
        sound[i] = 0.3f + 0.5f * (float) std::sin (twoPi * 200.0 * (double) i / testRate);

    auto played = sound;
    const auto takenOn = length / 4, letGo = length / 2, blockSize = 100;

    for (int done = 0; done < length; done += blockSize)
        switched.process (played.data() + done, std::min (blockSize, length - done), done >= takenOn && done < letGo, false);

    CHECK (std::equal (played.begin(), played.begin() + takenOn, sound.begin()));
    CHECK (std::equal (played.begin() + letGo + (int) (0.006 * testRate), played.end(), sound.begin() + letGo + (int) (0.006 * testRate)));
    CHECK (std::abs (std::accumulate (played.begin() + letGo - 2400, played.begin() + letGo, 0.0) / 2400.0) < 0.01); // settled: gone

    auto steepestSound = 0.0f, steepestPlayed = 0.0f;

    for (size_t i = 1; i < sound.size(); ++i)
    {
        steepestSound = std::max (steepestSound, std::abs (sound[i] - sound[i - 1]));
        steepestPlayed = std::max (steepestPlayed, std::abs (played[i] - played[i - 1]));
    }

    CHECK (steepestPlayed < steepestSound + 0.3f / 240.0f + 1.0e-3f);
}
