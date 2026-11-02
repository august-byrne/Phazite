// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's own devices (roadmap step 1.6): each one's engine-neutral DSP (src/devices/) in a DevicePlugin.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"
#include "model/base/internal/Tracktion.h"

#include <memory>
#include <span>
#include <string_view>

namespace phazite
{
// Registers each of Phazite's devices with the engine, so that projects can hold them. createEngine() calls it.
void registerDevices (te::Engine&);

// Phazite's devices, in the order menus list them.
std::span<const devices::DeviceInfo* const> getOwnDevices();

// One of Phazite's devices by its type ID (DeviceInfo::typeID), or nullptr; and a new one's DSP, for Phazite's own engine
// (model/playback/), or nullptr.
const devices::DeviceInfo* findOwnDevice (std::string_view typeID);
std::unique_ptr<devices::Processor> createOwnDevice (std::string_view typeID);

// Whether a device is an instrument, which turns notes into sound. Tracktion counts a rack as a synth, but Phazite's
// containers (model/mixer/internal/Containers.h) hold effects.
bool isInstrument (te::Plugin&);
} // namespace phazite
