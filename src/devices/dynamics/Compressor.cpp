// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Compressor.h"
#include "devices/dsp/Math.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
namespace
{
    // The IDs are saved in projects: never change them.
    constexpr ParameterInfo parameters[] = {
        { .id = "threshold", .name = "Threshold", .minimum = -60.0f, .maximum = 0.0f, .defaultValue = -18.0f, .unit = Unit::decibels },
        { .id = "ratio", .name = "Ratio", .minimum = 1.0f, .maximum = 20.0f, .defaultValue = 4.0f, .unit = Unit::ratio, .midpoint = 4.0f },
        { .id = "attack",
          .name = "Attack",
          .minimum = 0.1f,
          .maximum = 200.0f,
          .defaultValue = 10.0f,
          .unit = Unit::milliseconds,
          .midpoint = 10.0f },
        { .id = "release",
          .name = "Release",
          .minimum = 5.0f,
          .maximum = 2000.0f,
          .defaultValue = 120.0f,
          .unit = Unit::milliseconds,
          .midpoint = 100.0f },
        { .id = "knee", .name = "Knee", .minimum = 0.0f, .maximum = 24.0f, .defaultValue = 6.0f, .unit = Unit::decibels },
        { .id = "makeup", .name = "Makeup", .minimum = -12.0f, .maximum = 24.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
        { .id = "sidechainLowCut",
          .name = "Sidechain Low Cut",
          .minimum = 20.0f,
          .maximum = 500.0f,
          .defaultValue = 20.0f,
          .unit = Unit::hertz,
          .midpoint = 100.0f },
        { .id = mixParameterID, .name = "Mix", .defaultValue = 1.0f, .unit = Unit::percent },
    };

    // In a mixer slot: how loud it starts and how hard it pushes.
    constexpr std::string_view keyControls[] = { "threshold", "ratio" };

    constexpr double makeupSeconds = 0.02; // how long a change of the makeup gain takes, about

    // How many decibels a level goes down by: (1 - 1 / ratio) of the way it goes over the threshold, easing in across
    // the knee, which is centred on the threshold (Giannoulis, Massberg and Reiss, "Digital Dynamic Range Compressor
    // Design: A Tutorial and Analysis", 2012).
    float computeReduction (float level, float threshold, float slope, float knee) noexcept
    {
        const auto over = level - threshold;

        if (2.0f * over <= -knee)
            return 0.0f;

        if (2.0f * over < knee) // within the knee, which is wider than 0 here
        {
            const auto into = over + 0.5f * knee;
            return slope * into * into / (2.0f * knee);
        }

        return slope * over;
    }

    float getSlope (float ratio) noexcept
    {
        return 1.0f - 1.0f / std::max (ratio, 1.0f);
    }
} // namespace

const DeviceInfo Compressor::info { .typeID = typeID,
                                    .name = "Compressor",
                                    .kind = DeviceKind::effect,
                                    .parameters = parameters,
                                    .keyControls = keyControls,
                                    .hasSidechain = true };

float Compressor::getReduction (std::span<const float> values, float levelDecibels)
{
    return computeReduction (levelDecibels, values[threshold], getSlope (values[ratio]), values[knee]);
}

void Compressor::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    reduction.prepare (sampleRate);
    makeupSmoothing = (float) (1.0 - std::exp (-1.0 / (makeupSeconds * sampleRate)));
    reset();
}

void Compressor::reset()
{
    reduction.reset();

    for (auto& filter : lowCuts)
        filter.reset();

    isFirstBlock = true;
}

void Compressor::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);

    // What the detector hears: the sidechain if one feeds it, else the sound itself.
    const float* const* heard = block.channels;
    auto numHeard = numChannels;

    if (block.sidechain != nullptr && block.numSidechainChannels > 0)
    {
        heard = block.sidechain;
        numHeard = std::min (block.numSidechainChannels, 2);
    }

    const auto thresholdDecibels = values[threshold], kneeDecibels = values[knee], targetMakeup = values[makeup];
    const auto slope = getSlope (values[ratio]);
    const auto lowCut = dsp::makeFilter (dsp::FilterShape::highPass, values[sidechainLowCut], sampleRate, 0.70710678);
    reduction.setTimes (values[attack] * 0.001f, values[release] * 0.001f);

    if (isFirstBlock)
    {
        makeupDecibels = targetMakeup;
        isFirstBlock = false;
    }

    auto loudest = -120.0f, most = 0.0f;

    for (int i = 0; i < block.numSamples; ++i)
    {
        auto peak = 0.0f;

        for (int channel = 0; channel < numHeard; ++channel)
            peak = std::max (peak, std::abs (lowCuts[(size_t) channel].process (heard[channel][i], lowCut)));

        const auto level = dsp::gainToDecibels (peak);
        const auto amount = reduction.process (computeReduction (level, thresholdDecibels, slope, kneeDecibels));
        makeupDecibels += (targetMakeup - makeupDecibels) * makeupSmoothing;
        const auto gain = dsp::decibelsToGain (makeupDecibels - amount);

        for (int channel = 0; channel < numChannels; ++channel)
            block.channels[channel][i] *= gain;

        loudest = std::max (loudest, level);
        most = std::max (most, amount);
    }

    levelMeter.raise (loudest);
    reductionMeter.raise (most);
}
} // namespace phazite::devices
