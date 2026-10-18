// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Delay.h"
#include "devices/dsp/Math.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
namespace
{
    // The note values a synced echo takes, and their lengths in beats (quarter notes): T for a triplet, D for dotted.
    constexpr std::string_view noteValues[] = { "1/32", "1/16T", "1/16", "1/16D", "1/8T", "1/8",  "1/8D",
                                                "1/4T", "1/4",   "1/4D", "1/2T",  "1/2",  "1/2D", "1 bar" };
    constexpr double noteBeats[] = { 0.125, 1.0 / 6.0, 0.25, 0.375, 1.0 / 3.0, 0.5, 0.75, 2.0 / 3.0, 1.0, 1.5, 4.0 / 3.0, 2.0, 3.0, 4.0 };
    static_assert (std::size (noteValues) == std::size (noteBeats));

    // The IDs are saved in projects: never change them.
    constexpr ParameterInfo parameters[] = {
        { .id = "sync", .name = "Sync", .maximum = 1.0f, .defaultValue = 1.0f, .unit = Unit::toggle },
        { .id = "time",
          .name = "Time",
          .maximum = (float) std::size (noteValues) - 1.0f,
          .defaultValue = 6.0f, // a dotted eighth
          .unit = Unit::choice,
          .choices = noteValues },
        { .id = "milliseconds",
          .name = "Milliseconds",
          .minimum = 1.0f,
          .maximum = 2000.0f,
          .defaultValue = 375.0f,
          .unit = Unit::milliseconds,
          .midpoint = 250.0f },
        { .id = "feedback", .name = "Feedback", .maximum = 0.95f, .defaultValue = 0.35f, .unit = Unit::percent },
        { .id = "pingPong", .name = "Ping-Pong", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::toggle },
        { .id = "lowCut",
          .name = "Low Cut",
          .minimum = 20.0f,
          .maximum = 2000.0f,
          .defaultValue = 100.0f,
          .unit = Unit::hertz,
          .midpoint = 200.0f },
        { .id = "highCut",
          .name = "High Cut",
          .minimum = 1000.0f,
          .maximum = 20000.0f,
          .defaultValue = 8000.0f,
          .unit = Unit::hertz,
          .midpoint = 5000.0f },
        { .id = mixParameterID, .name = "Mix", .defaultValue = 0.25f, .unit = Unit::percent },
    };

    // In a mixer slot: how long the echoes take, and how many there are.
    constexpr std::string_view keyControls[] = { "time", "feedback" };

    constexpr double glideSeconds = 0.05; // how long a change of time takes to glide, about
} // namespace

const DeviceInfo Delay::info { .typeID = typeID,
                               .name = "Delay",
                               .kind = DeviceKind::effect,
                               .parameters = parameters,
                               .keyControls = keyControls };

double Delay::getDelaySeconds (std::span<const float> values, double tempo)
{
    auto seconds = (double) values[milliseconds] * 0.001;

    if (values[sync] > 0.5f)
    {
        const auto index = (size_t) juce::jlimit (0, (int) std::size (noteBeats) - 1, (int) std::lround (values[time]));
        seconds = noteBeats[index] * 60.0 / std::max (tempo, 1.0);
    }

    return std::min (seconds, maximumSeconds);
}

void Delay::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    timeSmoothing = (float) (1.0 - std::exp (-1.0 / (glideSeconds * sampleRate)));

    for (auto& line : lines)
        line.prepare ((int) std::ceil (maximumSeconds * sampleRate) + 1);

    reset();
}

void Delay::reset()
{
    for (auto& line : lines)
        line.reset();

    for (auto* filters : { &lowCuts, &highCuts })
        for (auto& filter : *filters)
            filter.reset();

    isFirstBlock = true;
}

void Delay::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);

    if (numChannels <= 0)
        return;

    const auto seconds = getDelaySeconds (values, block.tempo);
    const auto target = (float) juce::jlimit (1.0, maximumSeconds * sampleRate, seconds * sampleRate);

    if (isFirstBlock)
    {
        delaySamples = target;
        isFirstBlock = false;
    }

    const auto amount = values[feedback];
    const auto bounce = values[pingPong] > 0.5f && numChannels == 2;
    const auto lowCutFilter = dsp::makeFilter (dsp::FilterShape::highPass, values[lowCut], sampleRate, 0.70710678);
    const auto highCutFilter = dsp::makeFilter (dsp::FilterShape::lowPass, values[highCut], sampleRate, 0.70710678);

    // How long the echoes ring: until the feedback has taken them 60 dB down.
    const auto repeats = amount > 0.001f ? std::log (0.001) / std::log ((double) amount) : 1.0;
    tailSeconds.store (std::min (60.0, seconds * (repeats + 1.0)), std::memory_order_relaxed);

    auto* left = block.channels[0];
    auto* right = numChannels > 1 ? block.channels[1] : nullptr;

    for (int i = 0; i < block.numSamples; ++i)
    {
        delaySamples += (target - delaySamples) * timeSmoothing;

        // Each echo darker and thinner than the one before; the feedback saturates softly, so it never runs away.
        const auto echoLeft = highCuts[0].process (lowCuts[0].process (lines[0].read (delaySamples), lowCutFilter), highCutFilter);
        const auto echoRight =
            right != nullptr ? highCuts[1].process (lowCuts[1].process (lines[1].read (delaySamples), lowCutFilter), highCutFilter) : 0.0f;
        const auto backLeft = dsp::softClip (echoLeft * amount), backRight = dsp::softClip (echoRight * amount);
        const auto inLeft = left[i], inRight = right != nullptr ? right[i] : 0.0f;

        if (bounce)
        {
            // The sound goes in on the left, and each echo crosses to the other side.
            lines[0].push (0.5f * (inLeft + inRight) + backRight);
            lines[1].push (backLeft);
        }
        else
        {
            lines[0].push (inLeft + backLeft);

            if (right != nullptr)
                lines[1].push (inRight + backRight);
        }

        left[i] = echoLeft;

        if (right != nullptr)
            right[i] = echoRight;
    }
}
} // namespace phazite::devices
