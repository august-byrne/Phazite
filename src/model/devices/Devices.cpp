// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Devices.h"
#include "DevicePlugin.h"
#include "devices/eq/Equalizer.h"

namespace phazite
{
void registerDevices (te::Engine& engine)
{
    auto& plugins = engine.getPluginManager();
    plugins.createBuiltInType<DevicePluginOf<devices::Equalizer>>();
}
} // namespace phazite
