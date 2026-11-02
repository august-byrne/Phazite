// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceNode.h"

namespace phazite::engine
{
DeviceNode::DeviceNode (const devices::DeviceInfo& info, std::unique_ptr<devices::Processor> processor)
    : runner (info, std::move (processor)),
      values (std::make_unique<std::atomic<float>[]> (info.parameters.size())),
      blockValues (info.parameters.size())
{
    for (size_t i = 0; i < info.parameters.size(); ++i)
        values[i].store (info.parameters[i].defaultValue, std::memory_order_relaxed);
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

void DeviceNode::setOn (bool shouldBeOn) noexcept
{
    on.store (shouldBeOn, std::memory_order_relaxed);
}

void DeviceNode::process (juce::AudioBuffer<float>& buffer, int start, int numSamples, DeviceRunner::Block block) noexcept
{
    const auto isOnNow = isOn();

    if (isOnNow && ! wasOn)
        runner.reset();

    wasOn = isOnNow;

    if (! isOnNow)
        return;

    for (size_t i = 0; i < blockValues.size(); ++i)
        blockValues[i] = values[i].load (std::memory_order_relaxed);

    block.values = blockValues;
    runner.process (buffer, start, numSamples, block);
}
} // namespace phazite::engine
