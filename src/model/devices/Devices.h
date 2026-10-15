// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's own devices (roadmap step 1.6): each one's engine-neutral DSP (src/devices/) in a DevicePlugin.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "model/base/Tracktion.h"

#include <span>

namespace phazite
{
// Registers each of Phazite's devices with the engine, so that projects can hold them. createEngine() calls it.
void registerDevices (te::Engine&);

// Phazite's devices, in the order menus list them.
std::span<const devices::DeviceInfo* const> getOwnDevices();
} // namespace phazite
