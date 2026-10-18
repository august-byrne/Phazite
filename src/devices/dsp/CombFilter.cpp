// SPDX-License-Identifier: AGPL-3.0-or-later
#include "CombFilter.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <complex>

namespace phazite::devices::dsp
{
namespace
{
    // What the loop loses each time round at the most resonance: 2 % (0.18 dB), so that a comb at 440 Hz takes about
    // 0.8 s to fall by 60 dB.
    constexpr double leastLoss = 0.02;
} // namespace

// With what the loop loses each time round, l = 1 - |feedback|, the comb is
//     H(z) = output * l * (1 + sign z^-D) / (1 - feedback z^-D)
// so its peaks (where sign z^-D = 1) have a gain of 2 output, and its notches (where sign z^-D = -1) none. An output of
// l^-1/4 / 2 makes the peaks rise by as many decibels as white noise, spread over the whole comb, falls.
CombCoefficients makeComb (bool negative, double frequency, double sampleRate, float resonance)
{
    // Each step of resonance divides what the loop loses by the same factor, so that each sharpens the peaks as much.
    const auto loss = std::pow (leastLoss, std::clamp ((double) resonance, 0.0, 1.0));
    const auto period = sampleRate / std::max (frequency, 1.0);

    CombCoefficients c;
    c.delay = (float) std::max (2.0, negative ? 0.5 * period : period);
    c.feedback = (float) (negative ? loss - 1.0 : 1.0 - loss);
    c.input = (float) loss;
    c.output = (float) (0.5 * std::pow (loss, -0.25));
    c.sign = negative ? -1.0f : 1.0f;
    return c;
}

double getMagnitude (const CombCoefficients& c, double frequency, double sampleRate)
{
    const auto z = std::polar (1.0, -juce::MathConstants<double>::twoPi * frequency * (double) c.delay / sampleRate);
    return std::abs ((double) c.output * (double) c.input * (1.0 + (double) c.sign * z) / (1.0 - (double) c.feedback * z));
}

double getRingSeconds (const CombCoefficients& c, double sampleRate)
{
    // Each time round it falls by this many decibels; one delay more for what goes straight through it.
    const auto fallPerTrip = -20.0 * std::log10 (std::max ((double) std::abs (c.feedback), 1.0e-6));
    return (60.0 / fallPerTrip + 1.0) * (double) c.delay / sampleRate;
}

int getLongestCombDelay (double lowestFrequency, double sampleRate)
{
    return (int) std::ceil (sampleRate / std::max (lowestFrequency, 1.0));
}

//==============================================================================
void CombFilter::prepare (int maximumDelay)
{
    // The interpolating read reaches two samples past the longest delay.
    auto size = 4;

    while (size < maximumDelay + 3)
        size *= 2;

    buffer.assign ((size_t) size, 0.0f);
    mask = size - 1;
    writeIndex = 0;
    longest = (float) std::max (2, maximumDelay);
    used = false;
    fresh = true;
    stepsLeft = 0;
}

void CombFilter::reset() noexcept
{
    // Only if something was written: voices reset their combs at every note, most of them unused.
    if (used)
        std::fill (buffer.begin(), buffer.end(), 0.0f);

    used = false;
    fresh = true;
    stepsLeft = 0;
}

void CombFilter::setTarget (const CombCoefficients& next, int samples) noexcept
{
    target = next;
    target.delay = std::clamp (target.delay, 2.0f, longest);

    if (fresh || samples <= 1)
    {
        now = target;
        stepsLeft = 0;
        fresh = false;
        return;
    }

    const auto scale = 1.0f / (float) samples;
    step.delay = (target.delay - now.delay) * scale;
    step.feedback = (target.feedback - now.feedback) * scale;
    step.input = (target.input - now.input) * scale;
    step.output = (target.output - now.output) * scale;
    now.sign = target.sign;
    stepsLeft = samples;
}
} // namespace phazite::devices::dsp
