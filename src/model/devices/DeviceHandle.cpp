// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceHandle.h"
#include "DevicePlugin.h"
#include "DrumKit.h"
#include "internal/DeviceHandle.h"
#include "internal/ParameterHandle.h"
#include "model/mixer/Sidechain.h"

namespace phazite
{
struct DeviceHandle::Impl
{
    te::SafeSelectable<te::Plugin> plugin;
};

namespace
{
    te::Plugin* pluginOf (const std::shared_ptr<const DeviceHandle::Impl>& impl)
    {
        return impl != nullptr ? impl->plugin.get() : nullptr;
    }
} // namespace

DeviceHandle makeDeviceHandle (te::Plugin& p)
{
    if (dynamic_cast<DevicePlugin*> (&p) == nullptr)
        return {};

    auto impl = std::make_shared<DeviceHandle::Impl>();
    impl->plugin = &p;
    return DeviceHandle (std::move (impl));
}

DeviceHandle::DeviceHandle (std::shared_ptr<const Impl> i)
    : impl (std::move (i))
{
}

bool DeviceHandle::isValid() const
{
    return impl != nullptr && impl->plugin.get() != nullptr;
}

const devices::DeviceInfo* DeviceHandle::getInfo() const
{
    if (auto* device = dynamic_cast<DevicePlugin*> (pluginOf (impl)))
        return &device->getDeviceInfo();

    return nullptr;
}

ParameterHandle DeviceHandle::getParameter (std::string_view id, const juce::String& undoName) const
{
    if (auto* device = pluginOf (impl))
        return makeParameterHandle (device, juce::String (id.data(), id.size()), undoName);

    return {};
}

std::vector<DeviceHandle::SidechainChoice> DeviceHandle::getSidechainChoices() const
{
    std::vector<SidechainChoice> choices;

    if (auto* device = pluginOf (impl))
        for (auto* track : getSidechainSources (*device))
            choices.push_back ({ getTrackID (*track), track->getName() });

    return choices;
}

DeviceHandle::SidechainChoice DeviceHandle::getSidechainSource() const
{
    if (auto* device = pluginOf (impl))
        if (auto* track = phazite::getSidechainSource (*device))
            return { getTrackID (*track), track->getName() };

    return {};
}

void DeviceHandle::setSidechainSource (TrackID id) const
{
    if (auto* device = pluginOf (impl))
    {
        auto* track = id.isValid() ? te::findTrackForID (device->edit, toTracktion (id)) : nullptr;
        phazite::setSidechainSource (*device, dynamic_cast<te::AudioTrack*> (track));
    }
}

juce::String DeviceHandle::getPadName (int pad) const
{
    auto* device = pluginOf (impl);
    return device != nullptr ? phazite::getPadName (*device, pad) : juce::String();
}

bool DeviceHandle::hasPadSound (int pad) const
{
    auto* device = pluginOf (impl);
    return device != nullptr && phazite::hasPadSound (*device, pad);
}

bool DeviceHandle::setPadSound (int pad, const juce::File& file) const
{
    auto* device = pluginOf (impl);
    return device != nullptr && phazite::setPadSound (*device, pad, file);
}

void DeviceHandle::clearPadSound (int pad) const
{
    if (auto* device = pluginOf (impl))
        phazite::clearPadSound (*device, pad);
}

devices::Processor* DeviceHandle::getProcessorBase() const
{
    if (auto* device = dynamic_cast<DevicePlugin*> (pluginOf (impl)))
        return &device->getProcessor();

    return nullptr;
}
} // namespace phazite
