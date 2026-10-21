// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceHandle.h"
#include "internal/DeviceHandle.h"
#include "internal/DevicePlugin.h"
#include "internal/DrumKit.h"
#include "internal/ParameterHandle.h"
#include "internal/Wavetables.h"
#include "model/mixer/internal/Sidechain.h"

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

SidechainChoice DeviceHandle::getSidechainSource() const
{
    auto* device = pluginOf (impl);
    return device != nullptr ? describeSidechainSource (*device) : SidechainChoice();
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

juce::String DeviceHandle::getWavetableName (int osc) const
{
    auto* device = pluginOf (impl);
    return device != nullptr ? phazite::getWavetableName (*device, osc) : juce::String();
}

bool DeviceHandle::hasWavetableFile (int osc) const
{
    auto* device = pluginOf (impl);
    return device != nullptr && phazite::hasWavetableFile (*device, osc);
}

std::shared_ptr<const devices::Wavetable> DeviceHandle::getWavetable (int osc) const
{
    auto* device = pluginOf (impl);
    return device != nullptr ? phazite::getWavetable (*device, osc) : nullptr;
}

void DeviceHandle::setFactoryWavetable (int osc, int index) const
{
    if (auto* device = pluginOf (impl))
        phazite::setFactoryWavetable (*device, osc, index);
}

bool DeviceHandle::setWavetableFile (int osc, const juce::File& file) const
{
    auto* device = pluginOf (impl);
    return device != nullptr && phazite::setWavetableFile (*device, osc, file);
}

devices::DeviceScope* DeviceHandle::getScope() const
{
    if (auto* device = dynamic_cast<DevicePlugin*> (pluginOf (impl)))
        return &device->getScope();

    return nullptr;
}

devices::Processor* DeviceHandle::getProcessorBase() const
{
    if (auto* device = dynamic_cast<DevicePlugin*> (pluginOf (impl)))
        return &device->getProcessor();

    return nullptr;
}
} // namespace phazite
