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
    // The IDs are saved in projects.
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
        { .id = "keyLowCut",
          .name = "Key Low Cut",
          .minimum = 20.0f,
          .maximum = 500.0f,
          .defaultValue = 20.0f,
          .unit = Unit::hertz,
          .midpoint = 100.0f },
        { .id = "keyHighCut",
          .name = "Key High Cut",
          .minimum = 100.0f,
          .maximum = 20000.0f,
          .defaultValue = 20000.0f,
          .unit = Unit::hertz,
          .midpoint = 1500.0f },
        { .id = mixParameterID, .name = "Mix", .defaultValue = 1.0f, .unit = Unit::percent },
    };

    // In a mixer slot: how loud it starts and how hard it pushes.
    constexpr std::string_view keyControls[] = { "threshold", "ratio" };

    constexpr double makeupSeconds = 0.02;   // how long a change of the makeup gain takes, about
    constexpr float highCutOff = 19999.0f;   // the key's high cut at its top: off
    constexpr double keyFadeSeconds = 0.005; // between the key and the sound, as Listen turns on or off

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
    keyStep = (float) std::min (1.0, 1.0 / (keyFadeSeconds * sampleRate));
    reset();
}

void Compressor::reset()
{
    reduction.reset();

    for (auto& filter : lowCuts)
        filter.reset();

    for (auto& channel : highCuts)
        for (auto& filter : channel)
            filter.reset();

    isFirstBlock = true;
    keyShare = 0.0f;
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
    const auto lowCut = dsp::makeFilter (dsp::FilterShape::highPass, values[keyLowCut], sampleRate, 0.70710678);
    const auto cutsHigh = values[keyHighCut] < highCutOff;
    const std::array highCut { dsp::makeFilter (dsp::FilterShape::lowPass, values[keyHighCut], sampleRate, dsp::getButterworthQ (4, 0)),
                               dsp::makeFilter (dsp::FilterShape::lowPass, values[keyHighCut], sampleRate, dsp::getButterworthQ (4, 1)) };
    const auto keyWanted = isPlayingKey() ? 1.0f : 0.0f;
    reduction.setTimes (values[attack] * 0.001f, values[release] * 0.001f);

    if (isFirstBlock)
    {
        makeupDecibels = targetMakeup;
        isFirstBlock = false;
    }

    // Under the knee's start there is nothing to turn down, which the peak tells without its decibels; and the gain is
    // worked out again only when it changes (docs/performance-audit.md, A5).
    const auto kneeStartGain = dsp::decibelsToGain (thresholdDecibels - 0.5f * kneeDecibels);
    auto loudestPeak = 0.0f, most = 0.0f;

    for (int i = 0; i < block.numSamples; ++i)
    {
        // The key, through its cuts.
        auto peak = 0.0f;
        std::array<float, 2> key {};

        for (int channel = 0; channel < numHeard; ++channel)
        {
            auto& cut = highCuts[(size_t) channel];
            auto x = lowCuts[(size_t) channel].process (heard[channel][i], lowCut);

            if (cutsHigh)
                x = cut[1].process (cut[0].process (x, highCut[0]), highCut[1]);

            key[(size_t) channel] = x;
            peak = std::max (peak, std::abs (x));
        }

        const auto wanted =
            peak <= kneeStartGain ? 0.0f : computeReduction (dsp::gainToDecibels (peak), thresholdDecibels, slope, kneeDecibels);
        const auto amount = reduction.process (wanted);
        makeupDecibels += (targetMakeup - makeupDecibels) * makeupSmoothing;

        if (const auto gainDecibels = makeupDecibels - amount; ! juce::exactlyEqual (gainDecibels, lastGainDecibels))
        {
            lastGainDecibels = gainDecibels;
            lastGain = dsp::decibelsToGain (gainDecibels);
        }

        // The sound, turned down; or what it hears, to listen to, faded in and out.
        keyShare += std::clamp (keyWanted - keyShare, -keyStep, keyStep);

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const auto turnedDown = block.channels[channel][i] * lastGain;
            block.channels[channel][i] = turnedDown + keyShare * (key[(size_t) std::min (channel, numHeard - 1)] - turnedDown);
        }

        loudestPeak = std::max (loudestPeak, peak);
        most = std::max (most, amount);
    }

    levelMeter.raise (dsp::gainToDecibels (loudestPeak));
    reductionMeter.raise (most);
}
} // namespace phazite::devices
