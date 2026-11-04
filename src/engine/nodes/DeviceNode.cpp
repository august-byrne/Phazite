// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceNode.h"

#include <cmath>

namespace phazite::engine
{
DeviceNode::DeviceNode (const devices::DeviceInfo& info, std::unique_ptr<devices::Processor> processor)
    : runner (info, std::move (processor)),
      values (std::make_unique<std::atomic<float>[]> (info.parameters.size())),
      played (std::make_unique<std::atomic<float>[]> (info.parameters.size())),
      playedBase (std::make_unique<std::atomic<float>[]> (info.parameters.size())),
      writing (std::make_unique<std::atomic<bool>[]> (info.parameters.size())),
      blockValues (info.parameters.size())
{
    automated.reserve (info.parameters.size());

    for (size_t i = 0; i < info.parameters.size(); ++i)
    {
        for (auto* value : { &values[i], &played[i], &playedBase[i] })
            value->store (info.parameters[i].defaultValue, std::memory_order_relaxed);

        writing[i].store (false, std::memory_order_relaxed);
    }
}

void DeviceNode::setValue (int parameter, float value) noexcept
{
    const auto& parameters = getDeviceInfo().parameters;

    if (juce::isPositiveAndBelow (parameter, (int) parameters.size()))
        values[(size_t) parameter].store (devices::constrain (parameters[(size_t) parameter], value), std::memory_order_relaxed);
}

float DeviceNode::getValue (int parameter) const noexcept
{
    return juce::isPositiveAndBelow (parameter, (int) blockValues.size()) ? values[(size_t) parameter].load (std::memory_order_relaxed)
                                                                          : 0.0f;
}

void DeviceNode::setWriting (int parameter, bool isWriting) noexcept
{
    if (juce::isPositiveAndBelow (parameter, (int) blockValues.size()))
        writing[(size_t) parameter].store (isWriting, std::memory_order_relaxed);
}

bool DeviceNode::isWriting (int parameter) const noexcept
{
    return juce::isPositiveAndBelow (parameter, (int) blockValues.size()) && writing[(size_t) parameter].load (std::memory_order_relaxed);
}

float DeviceNode::getPlayedValue (int parameter) const noexcept
{
    return juce::isPositiveAndBelow (parameter, (int) blockValues.size()) ? played[(size_t) parameter].load (std::memory_order_relaxed)
                                                                          : 0.0f;
}

float DeviceNode::getPlayedBaseValue (int parameter) const noexcept
{
    return juce::isPositiveAndBelow (parameter, (int) blockValues.size()) ? playedBase[(size_t) parameter].load (std::memory_order_relaxed)
                                                                          : 0.0f;
}

void DeviceNode::setOn (bool shouldBeOn) noexcept
{
    on.store (shouldBeOn, std::memory_order_relaxed);
}

bool DeviceNode::takeBlock (DeviceRunner::Block& block, std::span<const ParameterCurve> automation) noexcept
{
    const auto isOnNow = isOn();

    if (isOnNow && ! wasOn)
        runner.reset();

    wasOn = isOnNow;

    if (! isOnNow)
        return false;

    for (size_t i = 0; i < blockValues.size(); ++i)
        blockValues[i] = values[i].load (std::memory_order_relaxed);

    // Its automation, but where a parameter is written by hand (at most one curve a parameter: the reserve holds them).
    automated.clear();

    for (const auto& each : automation)
        if (juce::isPositiveAndBelow (each.parameter, (int) blockValues.size()) && automated.size() < blockValues.size()
            && ! writing[(size_t) each.parameter].load (std::memory_order_relaxed))
            automated.push_back ({ each.parameter, each.curve.get() });

    block.values = blockValues;
    block.automation = automated;
    return true;
}

void DeviceNode::start (juce::AudioBuffer<float>& buffer, int start, int numSamples, DeviceRunner::Block block,
                        std::span<const ParameterCurve> automation) noexcept
{
    const auto began = juce::Time::getHighResolutionTicks();
    started = takeBlock (block, automation);

    if (started)
        runner.start (buffer, start, numSamples, block);

    startTicks = juce::Time::getHighResolutionTicks() - began;
}

void DeviceNode::measure (std::int64_t ticks, int numSamples) noexcept
{
    const auto rate = runner.getSampleRate();

    if (numSamples <= 0 || rate <= 0.0)
        return;

    // As long a time at any block size: the shorter the block, the less of the average it moves.
    const auto blockSeconds = (double) numSamples / rate;
    const auto now = juce::Time::highResolutionTicksToSeconds (ticks) / blockSeconds;
    const auto kept = std::exp (-blockSeconds / loadSeconds);
    load.store ((float) (now + ((double) load.load (std::memory_order_relaxed) - now) * kept), std::memory_order_relaxed);
}

void DeviceNode::process (juce::AudioBuffer<float>& buffer, int start, int numSamples, DeviceRunner::Block block,
                          std::span<const ParameterCurve> automation) noexcept
{
    // Started (start()), with the values and automation it took then; or as it is now.
    const auto began = juce::Time::getHighResolutionTicks();
    const auto ticksBefore = std::exchange (startTicks, 0);

    if (std::exchange (started, false))
    {
        block.values = blockValues;
        block.automation = automated;
    }
    else if (! takeBlock (block, automation))
    {
        measure (0, numSamples);
        return;
    }

    runner.process (buffer, start, numSamples, block);
    measure (ticksBefore + juce::Time::getHighResolutionTicks() - began, numSamples);

    // What it played, for the controls.
    const auto playedValues = runner.getValues();
    const auto baseValues = runner.getBaseValues();

    for (size_t i = 0; i < blockValues.size(); ++i)
    {
        played[i].store (playedValues[i], std::memory_order_relaxed);
        playedBase[i].store (baseValues[i], std::memory_order_relaxed);
    }
}
} // namespace phazite::engine
