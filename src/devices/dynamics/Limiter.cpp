// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Limiter.h"
#include "devices/dsp/Math.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
namespace
{
    // The IDs are saved in projects.
    constexpr ParameterInfo parameters[] = {
        { .id = "gain", .name = "Gain", .minimum = 0.0f, .maximum = 24.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
        { .id = "ceiling", .name = "Ceiling", .minimum = -12.0f, .maximum = 0.0f, .defaultValue = -1.0f, .unit = Unit::decibels },
        { .id = "release",
          .name = "Release",
          .minimum = 1.0f,
          .maximum = 1000.0f,
          .defaultValue = 100.0f,
          .unit = Unit::milliseconds,
          .midpoint = 50.0f },
    };

    constexpr double gainSeconds = 0.02; // how long a change of the gain takes, about
} // namespace

const DeviceInfo Limiter::info { .typeID = typeID, .name = "Limiter", .kind = DeviceKind::effect, .parameters = parameters };

void Limiter::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    lookahead = std::max (1, (int) std::lround (lookaheadSeconds * sampleRate));

    // The gain for a sample is known once the samples up to lookahead - 1 after it have come in, and the ones after it
    // that the true peak's filter reads.
    latency = lookahead + TruePeak::pointAge - 1;
    gainSmoothing = (float) (1.0 - std::exp (-1.0 / (gainSeconds * sampleRate)));

    heldValues.assign ((size_t) lookahead, 1.0f);
    heldTimes.assign ((size_t) lookahead, 0);
    averaged.assign ((size_t) lookahead, 1.0f);

    for (auto& delay : delays)
        delay.prepare (latency);

    reset();
}

void Limiter::reset()
{
    for (auto& truePeak : truePeaks)
        truePeak.reset();

    lastBetween = 0.0f;
    heldFirst = heldCount = 0;
    now = 0;
    released = 1.0f;
    std::fill (averaged.begin(), averaged.end(), 1.0f);
    averagedNext = 0;
    averagedSum = (double) lookahead;

    for (auto& delay : delays)
        delay.reset();

    isFirstBlock = true;
}

// The least gain needed by the last lookahead samples: the values that could still be the least, oldest first.
float Limiter::hold (float required) noexcept
{
    const auto size = (int) heldValues.size();

    // The oldest leaves the window first, so that the new one has room.
    if (heldCount > 0 && heldTimes[(size_t) heldFirst] <= now - lookahead)
    {
        heldFirst = (heldFirst + 1) % size;
        --heldCount;
    }

    while (heldCount > 0 && heldValues[(size_t) ((heldFirst + heldCount - 1) % size)] >= required)
        --heldCount;

    heldValues[(size_t) ((heldFirst + heldCount) % size)] = required;
    heldTimes[(size_t) ((heldFirst + heldCount) % size)] = now;
    ++heldCount;
    ++now;
    return heldValues[(size_t) heldFirst];
}

// The average of the last lookahead values: each sample's gain ramps down in a straight line to what it needs.
float Limiter::average (float value) noexcept
{
    averagedSum += (double) value - (double) averaged[(size_t) averagedNext];
    averaged[(size_t) averagedNext] = value;
    averagedNext = (averagedNext + 1) % lookahead;

    if (averagedNext == 0) // adds it up afresh now and then, so rounding doesn't build up
    {
        averagedSum = 0.0;

        for (const auto held : averaged)
            averagedSum += (double) held;
    }

    return (float) (averagedSum / lookahead);
}

void Limiter::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);
    const auto targetGain = dsp::decibelsToGain (values[gain]);
    const auto ceilingGain = dsp::decibelsToGain (values[ceiling]);
    const auto releaseCoefficient = (float) (1.0 - std::exp (-1.0 / ((double) values[release] * 0.001 * sampleRate)));

    if (isFirstBlock)
    {
        appliedGain = targetGain;
        isFirstBlock = false;
    }

    auto loudestIn = 0.0f, loudestOut = 0.0f, least = 1.0f;

    for (int i = 0; i < block.numSamples; ++i)
    {
        appliedGain += (targetGain - appliedGain) * gainSmoothing;
        auto sample = 0.0f, between = 0.0f;

        for (int channel = 0; channel < numChannels; ++channel)
        {
            auto& truePeak = truePeaks[(size_t) channel];
            const auto in = block.channels[channel][i] * appliedGain;
            delays[(size_t) channel].push (in);
            loudestIn = std::max (loudestIn, std::abs (in));

            // The points between the sample whose gain is worked out now and the next, and that sample.
            between = std::max (between, truePeak.push (in));
            sample = std::max (sample, std::abs (truePeak.getSample (TruePeak::pointAge)));
        }

        // What that sample needs, with the peaks on either side of it, held, released and smoothed.
        const auto peak = std::max ({ sample, between, lastBetween });
        lastBetween = between;
        const auto held = hold (peak > ceilingGain ? ceilingGain / peak : 1.0f);
        released = held < released ? held : released + (held - released) * releaseCoefficient;
        const auto smoothed = average (released);
        least = std::min (least, smoothed);

        // The sample from latency ago, which that gain belongs to; the clip only catches rounding.
        for (int channel = 0; channel < numChannels; ++channel)
        {
            const auto out = std::clamp (delays[(size_t) channel].read (latency) * smoothed, -ceilingGain, ceilingGain);
            block.channels[channel][i] = out;
            loudestOut = std::max (loudestOut, std::abs (out));
        }
    }

    inputMeter.raise (dsp::gainToDecibels (loudestIn));
    reductionMeter.raise (-dsp::gainToDecibels (least));
    gainReduction.push (-dsp::gainToDecibels (least));
    outputMeter.raise (dsp::gainToDecibels (loudestOut));
}
} // namespace phazite::devices
