// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Maximizer.h"
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
        { .id = "gain", .name = "Gain", .maximum = 24.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
        { .id = "lowFrequency",
          .name = "Low Crossover",
          .minimum = 40.0f,
          .maximum = 1000.0f,
          .defaultValue = 120.0f,
          .unit = Unit::hertz,
          .midpoint = 200.0f },
        { .id = "highFrequency",
          .name = "High Crossover",
          .minimum = 1000.0f,
          .maximum = 16000.0f,
          .defaultValue = 5000.0f,
          .unit = Unit::hertz,
          .midpoint = 4000.0f },
        { .id = "lowSqueeze", .name = "Low Squeeze", .defaultValue = 0.3f, .unit = Unit::percent },
        { .id = "midSqueeze", .name = "Mid Squeeze", .defaultValue = 0.3f, .unit = Unit::percent },
        { .id = "highSqueeze", .name = "High Squeeze", .defaultValue = 0.3f, .unit = Unit::percent },
        { .id = "lowLevel", .name = "Low Level", .minimum = -12.0f, .maximum = 12.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
        { .id = "midLevel", .name = "Mid Level", .minimum = -12.0f, .maximum = 12.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
        { .id = "highLevel", .name = "High Level", .minimum = -12.0f, .maximum = 12.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
        { .id = "ceiling", .name = "Ceiling", .minimum = -12.0f, .maximum = 0.0f, .defaultValue = -1.0f, .unit = Unit::decibels },
        { .id = "release",
          .name = "Release",
          .minimum = 10.0f,
          .maximum = 1000.0f,
          .defaultValue = 100.0f,
          .unit = Unit::milliseconds,
          .midpoint = 100.0f },
        { .id = mixParameterID, .name = "Mix", .defaultValue = 1.0f, .unit = Unit::percent },
    };

    // In a mixer slot: how loud, and how far it may go.
    constexpr std::string_view keyControls[] = { "gain", "ceiling" };

    // Each band's compressor: above the threshold, across a soft knee, at a ratio from 1:1 (no squeeze) to 10:1.
    constexpr float threshold = Maximizer::squeezeThreshold, knee = 6.0f, mostRatio = 10.0f;
    constexpr float attacks[] = { 0.01f, 0.005f, 0.002f }; // seconds: the low band slower, as its waves are longer

    constexpr double smoothingSeconds = 0.02;
} // namespace

const DeviceInfo Maximizer::info { .typeID = typeID,
                                   .name = "Maximizer",
                                   .kind = DeviceKind::effect,
                                   .parameters = parameters,
                                   .keyControls = keyControls };

float Maximizer::getReduction (float squeeze, float level)
{
    const auto slope = 1.0f - 1.0f / (1.0f + std::clamp (squeeze, 0.0f, 1.0f) * (mostRatio - 1.0f));
    const auto over = level - threshold;

    if (slope <= 0.0f || 2.0f * over <= -knee)
        return 0.0f;

    if (2.0f * over < knee)
    {
        const auto into = over + 0.5f * knee;
        return slope * into * into / (2.0f * knee);
    }

    return slope * over;
}

void Maximizer::prepare (double newSampleRate, int maximumBlockSize)
{
    sampleRate = newSampleRate;
    smoothing = (float) (1.0 - std::exp (-1.0 / (smoothingSeconds * sampleRate)));

    for (auto& reduction : reductions)
        reduction.prepare (sampleRate);

    limiter.prepare (sampleRate, maximumBlockSize);
    shownLow = shownHigh = 0.0f;
    reset();
}

void Maximizer::reset()
{
    for (auto& split : splits)
        split.reset();

    for (auto& reduction : reductions)
        reduction.reset();

    limiter.reset();
    isFirstBlock = true;
}

void Maximizer::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);

    if (numChannels <= 0)
        return;

    if (std::abs (values[lowFrequency] - shownLow) > 0.01f || std::abs (values[highFrequency] - shownHigh) > 0.01f)
    {
        shownLow = values[lowFrequency];
        shownHigh = std::max (values[highFrequency], shownLow * 1.5f);

        for (auto& split : splits)
            split.setFrequencies (shownLow, shownHigh, sampleRate);
    }

    for (size_t band = 0; band < numBands; ++band)
        reductions[band].setTimes (attacks[band], values[release] * 0.001f);

    const std::array<float, numBands> squeezes { values[lowSqueeze], values[midSqueeze], values[highSqueeze] };
    const std::array<float, numBands> targetLevels { dsp::decibelsToGain (values[lowLevel]), dsp::decibelsToGain (values[midLevel]),
                                                     dsp::decibelsToGain (values[highLevel]) };
    const auto targetIn = dsp::decibelsToGain (values[gain]);

    if (isFirstBlock)
    {
        inGain = targetIn;
        levels = targetLevels;
        isFirstBlock = false;
    }

    // For the editor: each band's loudest, and the most it was squeezed, this block.
    std::array<float, numBands> loudest {}, mostReduced {};

    for (int i = 0; i < block.numSamples; ++i)
    {
        inGain += (targetIn - inGain) * smoothing;

        std::array<std::array<float, numBands>, 2> bands {};
        std::array<float, numBands> peaks {};

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const auto split = splits[(size_t) channel].process (block.channels[channel][i] * inGain);
            bands[(size_t) channel] = { split.low, split.mid, split.high };

            for (size_t band = 0; band < numBands; ++band)
                peaks[band] = std::max (peaks[band], std::abs (bands[(size_t) channel][band]));
        }

        // Each band squeezed on its own, both channels alike, then set to its level.
        std::array<float, numBands> gains {};

        for (size_t band = 0; band < numBands; ++band)
        {
            levels[band] += (targetLevels[band] - levels[band]) * smoothing;
            const auto reduced = reductions[band].process (getReduction (squeezes[band], dsp::gainToDecibels (peaks[band])));
            gains[band] = dsp::decibelsToGain (-reduced) * levels[band];
            loudest[band] = std::max (loudest[band], peaks[band]);
            mostReduced[band] = std::max (mostReduced[band], reduced);
        }

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const auto& split = bands[(size_t) channel];
            block.channels[channel][i] = split[0] * gains[0] + split[1] * gains[1] + split[2] * gains[2];
        }
    }

    for (size_t band = 0; band < numBands; ++band)
    {
        bandLevels[band].raise (dsp::gainToDecibels (loudest[band]));
        bandReductions[band].raise (mostReduced[band]);
    }

    // Then the limiter keeps it all under the ceiling.
    limiterValues[Limiter::gain] = 0.0f;
    limiterValues[Limiter::ceiling] = values[ceiling];
    limiterValues[Limiter::release] = values[release];
    limiterValues[Limiter::mix] = 1.0f;

    ProcessContext limited { block };
    limited.numChannels = numChannels;
    limited.parameters = limiterValues;
    limiter.process (limited);
}
} // namespace phazite::devices
