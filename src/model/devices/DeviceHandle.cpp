// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceHandle.h"
#include "DevicePlugin.h"
#include "model/mixer/Sidechain.h"

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

std::vector<DeviceHandle::SidechainChoice> DeviceHandle::getSidechainChoices() const
{
    std::vector<SidechainChoice> choices;

    if (auto* device = plugin.get())
        for (auto* track : getSidechainSources (*device))
            choices.push_back ({ track->itemID.getRawID(), track->getName() });

    return choices;
}

DeviceHandle::SidechainChoice DeviceHandle::getSidechainSource() const
{
    if (auto* device = plugin.get())
        if (auto* track = phazite::getSidechainSource (*device))
            return { track->itemID.getRawID(), track->getName() };

    return {};
}

void DeviceHandle::setSidechainSource (std::uint64_t trackID) const
{
    if (auto* device = plugin.get())
    {
        auto* track = trackID != 0 ? te::findTrackForID (device->edit, te::EditItemID::fromRawID (trackID)) : nullptr;
        phazite::setSidechainSource (*device, dynamic_cast<te::AudioTrack*> (track));
    }
}

devices::Processor* DeviceHandle::getProcessorBase() const
{
    if (auto* device = dynamic_cast<DevicePlugin*> (plugin.get()))
        return &device->getProcessor();

    return nullptr;
}
} // namespace phazite
