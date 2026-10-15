// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceHandle.h"
#include "DevicePlugin.h"

namespace phazite
{
DeviceHandle::DeviceHandle (te::Plugin& p)
    : plugin (dynamic_cast<DevicePlugin*> (&p) != nullptr ? &p : nullptr)
{
}

bool DeviceHandle::isValid() const
{
    return plugin.get() != nullptr;
}

const devices::DeviceInfo* DeviceHandle::getInfo() const
{
    if (auto* device = dynamic_cast<DevicePlugin*> (plugin.get()))
        return &device->getDeviceInfo();

    return nullptr;
}

ParameterHandle DeviceHandle::getParameter (std::string_view id, const juce::String& undoName) const
{
    if (auto* device = plugin.get())
        return ParameterHandle (device, juce::String (id.data(), id.size()), undoName);

    return {};
}

devices::Processor* DeviceHandle::getProcessorBase() const
{
    if (auto* device = dynamic_cast<DevicePlugin*> (plugin.get()))
        return &device->getProcessor();

    return nullptr;
}
} // namespace phazite
