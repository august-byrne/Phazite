// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Multiband.h"
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
        { .id = "depth", .name = "Depth", .defaultValue = 0.5f, .unit = Unit::percent },
        { .id = "time", .name = "Time", .defaultValue = 0.5f, .unit = Unit::percent },
        { .id = "upward", .name = "Upward", .defaultValue = 1.0f, .unit = Unit::percent },
        { .id = "downward", .name = "Downward", .defaultValue = 1.0f, .unit = Unit::percent },
        { .id = "lowFrequency",
          .name = "Low Crossover",
          .minimum = 40.0f,
          .maximum = 1000.0f,
          .defaultValue = 90.0f,
          .unit = Unit::hertz,
          .midpoint = 200.0f },
        { .id = "highFrequency",
          .name = "High Crossover",
          .minimum = 1000.0f,
          .maximum = 16000.0f,
          .defaultValue = 2500.0f,
          .unit = Unit::hertz,
          .midpoint = 4000.0f },
        { .id = "lowGain", .name = "Low Gain", .minimum = -24.0f, .maximum = 24.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
        { .id = "midGain", .name = "Mid Gain", .minimum = -24.0f, .maximum = 24.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
        { .id = "highGain", .name = "High Gain", .minimum = -24.0f, .maximum = 24.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
        { .id = "inputGain", .name = "Input Gain", .minimum = -24.0f, .maximum = 24.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
        { .id = "outputGain", .name = "Output Gain", .minimum = -24.0f, .maximum = 24.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
        { .id = mixParameterID, .name = "Mix", .defaultValue = 1.0f, .unit = Unit::percent },
    };

    // In a mixer slot: how much, and how fast.
    constexpr std::string_view keyControls[] = { "depth", "time" };

    // Each band is turned down hard above the downward threshold (10:1, across a soft knee) and brought up 3:1 below the
    // upward one, by at most maxBoost, the boost fading out toward silence so that noise isn't raised.
    constexpr float downThreshold = -30.0f, downSlope = 0.9f, knee = 6.0f;
    constexpr float upThreshold = -40.0f, upSlope = 2.0f / 3.0f, maxBoost = 18.0f;
    constexpr float boostFadeTop = -70.0f, boostFadeBottom = -90.0f;

    // How fast each band reacts at the middle time, in seconds: the low band slower, as its waves are longer.
    constexpr float attacks[] = { 0.02f, 0.01f, 0.005f };
    constexpr float releases[] = { 0.2f, 0.12f, 0.08f };

    constexpr double smoothingSeconds = 0.02;

    float getReduction (float level) noexcept
    {
        const auto over = level - downThreshold;

        if (2.0f * over <= -knee)
            return 0.0f;

        if (2.0f * over < knee)
        {
            const auto into = over + 0.5f * knee;
            return downSlope * into * into / (2.0f * knee);
        }

        return downSlope * over;
    }

    float getBoost (float level) noexcept
    {
        if (level >= upThreshold)
            return 0.0f;

        const auto boost = std::min (maxBoost, (upThreshold - level) * upSlope);
        const auto fade = std::clamp ((level - boostFadeBottom) / (boostFadeTop - boostFadeBottom), 0.0f, 1.0f);
        return boost * fade;
    }
} // namespace

const DeviceInfo Multiband::info { .typeID = typeID,
                                   .name = "Multiband",
                                   .kind = DeviceKind::effect,
                                   .parameters = parameters,
                                   .keyControls = keyControls };

float Multiband::getGainChange (std::span<const float> values, float level)
{
    return values[depth] * (values[upward] * getBoost (level) - values[downward] * getReduction (level));
}

void Multiband::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    smoothing = (float) (1.0 - std::exp (-1.0 / (smoothingSeconds * sampleRate)));

    for (auto& change : changes)
        change.prepare (sampleRate);

    shownLow = shownHigh = 0.0f;
    reset();
}

void Multiband::reset()
{
    for (auto& split : splits)
        split.reset();

    for (auto& change : changes)
        change.reset();

    isFirstBlock = true;
}

void Multiband::process (const ProcessContext& block)
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

    // Time scales every band's attack and release, a tenth as long at 0 and ten times at 100 %.
    const auto scale = std::pow (10.0f, (values[time] - 0.5f) * 2.0f);

    for (size_t band = 0; band < numBands; ++band)
        changes[band].setTimes (attacks[band] * scale, releases[band] * scale);

    const auto targetIn = dsp::decibelsToGain (values[inputGain]), targetOut = dsp::decibelsToGain (values[outputGain]);
    const std::array<float, numBands> targetBands { dsp::decibelsToGain (values[lowGain]), dsp::decibelsToGain (values[midGain]),
                                                    dsp::decibelsToGain (values[highGain]) };

    if (isFirstBlock)
    {
        inGain = targetIn;
        outGain = targetOut;
        bandGains = targetBands;
        isFirstBlock = false;
    }

    for (int i = 0; i < block.numSamples; ++i)
    {
        inGain += (targetIn - inGain) * smoothing;
        outGain += (targetOut - outGain) * smoothing;

        std::array<std::array<float, numBands>, 2> bands {};
        std::array<float, numBands> peaks {};

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const auto split = splits[(size_t) channel].process (block.channels[channel][i] * inGain);
            bands[(size_t) channel] = { split.low, split.mid, split.high };

            for (size_t band = 0; band < numBands; ++band)
                peaks[band] = std::max (peaks[band], std::abs (bands[(size_t) channel][band]));
        }

        // Each band's gain change, linked across the channels so the stereo image holds; the follower smooths the
        // reduction (a boost being a negative one), quickly as it grows and slowly as it lets go.
        std::array<float, numBands> gains {};

        for (size_t band = 0; band < numBands; ++band)
        {
            bandGains[band] += (targetBands[band] - bandGains[band]) * smoothing;
            const auto reduction = changes[band].process (-getGainChange (values, dsp::gainToDecibels (peaks[band])));
            gains[band] = dsp::decibelsToGain (-reduction) * bandGains[band];
        }

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const auto& split = bands[(size_t) channel];
            block.channels[channel][i] = (split[0] * gains[0] + split[1] * gains[1] + split[2] * gains[2]) * outGain;
        }
    }
}
} // namespace phazite::devices
