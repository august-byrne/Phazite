// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Utility.h"
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
        { .id = "gain", .name = "Gain", .minimum = -36.0f, .maximum = 36.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
        { .id = "balance", .name = "Balance", .minimum = -1.0f, .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::pan },
        { .id = "width", .name = "Width", .maximum = 2.0f, .defaultValue = 1.0f, .unit = Unit::percent },
        { .id = "bassMono", .name = "Bass Mono", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::toggle },
        { .id = "bassMonoFrequency",
          .name = "Bass Mono Frequency",
          .minimum = 40.0f,
          .maximum = 500.0f,
          .defaultValue = 120.0f,
          .unit = Unit::hertz,
          .midpoint = 140.0f },
        { .id = "invertLeft", .name = "Invert Left", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::toggle },
        { .id = "invertRight", .name = "Invert Right", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::toggle },
    };

    // In a mixer slot: level and width.
    constexpr std::string_view keyControls[] = { "gain", "width" };

    constexpr double smoothingSeconds = 0.02; // how long a change takes, about
} // namespace

const DeviceInfo Utility::info { .typeID = typeID,
                                 .name = "Utility",
                                 .kind = DeviceKind::effect,
                                 .parameters = parameters,
                                 .keyControls = keyControls };

void Utility::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    smoothing = (float) (1.0 - std::exp (-1.0 / (smoothingSeconds * sampleRate)));
    shownFrequency = 0.0f;
    reset();
}

void Utility::reset()
{
    for (auto& crossover : crossovers)
        crossover.reset();

    isFirstBlock = true;
}

void Utility::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);

    if (numChannels <= 0)
        return;

    // The gain, the balance (a side turned down as the other is favoured, as a stereo track's balance does), and each
    // side's polarity, in one gain per side.
    const auto level = dsp::decibelsToGain (values[gain]);
    const auto targetLeft = level * std::min (1.0f, 1.0f - values[balance]) * (values[invertLeft] > 0.5f ? -1.0f : 1.0f);
    const auto targetRight = level * std::min (1.0f, 1.0f + values[balance]) * (values[invertRight] > 0.5f ? -1.0f : 1.0f);
    const auto targetSide = values[width];
    const auto mono = values[bassMono] > 0.5f;

    if (isFirstBlock)
    {
        leftGain = targetLeft;
        rightGain = targetRight;
        sideGain = targetSide;
        isFirstBlock = false;
    }

    if (mono && std::abs (values[bassMonoFrequency] - shownFrequency) > 0.01f)
    {
        shownFrequency = values[bassMonoFrequency];

        for (auto& crossover : crossovers)
            crossover.setFrequency (shownFrequency, sampleRate);
    }

    auto* left = block.channels[0];
    auto* right = numChannels > 1 ? block.channels[1] : nullptr;

    for (int i = 0; i < block.numSamples; ++i)
    {
        leftGain += (targetLeft - leftGain) * smoothing;
        rightGain += (targetRight - rightGain) * smoothing;
        sideGain += (targetSide - sideGain) * smoothing;

        if (right == nullptr)
        {
            left[i] *= leftGain;
            continue;
        }

        auto l = left[i], r = right[i];

        // The bass, below the crossover, in mono; what is above it as it was.
        if (mono)
        {
            const auto lowLeft = crossovers[0].process (l), lowRight = crossovers[1].process (r);
            const auto bass = 0.5f * (lowLeft.low + lowRight.low);
            l = bass + lowLeft.high;
            r = bass + lowRight.high;
        }

        // The width, through mid and side: 0 is mono, 1 as it was, 2 twice as wide.
        const auto mid = 0.5f * (l + r), side = 0.5f * (l - r) * sideGain;
        left[i] = (mid + side) * leftGain;
        right[i] = (mid - side) * rightGain;
    }
}
} // namespace phazite::devices
