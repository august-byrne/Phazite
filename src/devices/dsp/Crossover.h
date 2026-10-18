// SPDX-License-Identifier: AGPL-3.0-or-later
// Crossovers for Phazite's devices: Linkwitz-Riley splits of the fourth order (two Butterworth sections each way, 24 dB
// per octave), whose low and high outputs add up to the input passed through an all-pass, so bands split by them sum
// back to the input's level at every frequency (Linkwitz and Riley, 1976). Three bands come from two crossovers, with the
// low band passed through the second one's all-pass so that it stays in phase with the others. Real-time safe.
#pragma once

#include "Filters.h"

namespace phazite::devices::dsp
{
// One channel's split into two bands at a frequency.
class Crossover
{
public:
    void setFrequency (double frequency, double sampleRate) noexcept
    {
        constexpr double butterworthQ = 0.70710678;
        low = makeFilter (FilterShape::lowPass, frequency, sampleRate, butterworthQ);
        high = makeFilter (FilterShape::highPass, frequency, sampleRate, butterworthQ);
        allPass = makeFilter (FilterShape::allPass, frequency, sampleRate, butterworthQ);
    }

    void reset() noexcept
    {
        for (auto* filter : { &low1, &low2, &high1, &high2, &phase })
            filter->reset();
    }

    struct Bands
    {
        float low, high;
    };

    Bands process (float input) noexcept
    {
        return { low2.process (low1.process (input, low), low), high2.process (high1.process (input, high), high) };
    }

    // What the two bands add up to, without splitting: for a band that goes around this crossover, to stay in phase.
    float processAllPass (float input) noexcept { return phase.process (input, allPass); }

private:
    FilterCoefficients low, high, allPass;
    Filter low1, low2, high1, high2, phase;
};

// One channel's split into three bands at two frequencies, the first below the second.
class ThreeBandSplit
{
public:
    void setFrequencies (double lowMid, double midHigh, double sampleRate) noexcept
    {
        first.setFrequency (lowMid, sampleRate);
        second.setFrequency (midHigh, sampleRate);
    }

    void reset() noexcept
    {
        first.reset();
        second.reset();
    }

    struct Bands
    {
        float low, mid, high;
    };

    Bands process (float input) noexcept
    {
        const auto split = first.process (input);
        const auto upper = second.process (split.high);
        return { second.processAllPass (split.low), upper.low, upper.high };
    }

private:
    Crossover first, second;
};
} // namespace phazite::devices::dsp
