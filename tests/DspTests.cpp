// SPDX-License-Identifier: AGPL-3.0-or-later
// The building blocks of Phazite's devices' DSP (src/devices/dsp/): filters, comb filters, envelopes, oscillators, random
// numbers, delay lines.
#include "TestHelpers.h"
#include "devices/dsp/CombFilter.h"
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
