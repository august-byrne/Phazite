// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Devices.h"
#include "DevicePlugin.h"
#include "devices/dynamics/Compressor.h"
#include "devices/dynamics/Limiter.h"
#include "devices/eq/Equalizer.h"
#include "devices/reverb/Reverb.h"

namespace phazite
{
void registerDevices (te::Engine& engine)
{
    auto& plugins = engine.getPluginManager();
    plugins.createBuiltInType<DevicePluginOf<devices::Equalizer>>();
    plugins.createBuiltInType<DevicePluginOf<devices::Compressor>>();
    plugins.createBuiltInType<DevicePluginOf<devices::Limiter>>();
    plugins.createBuiltInType<DevicePluginOf<devices::Reverb>>();
}

std::span<const devices::DeviceInfo* const> getOwnDevices()
{
    static const devices::DeviceInfo* const own[] = { &devices::Equalizer::info, &devices::Compressor::info, &devices::Limiter::info,
                                                      &devices::Reverb::info };
    return own;
}
} // namespace phazite
