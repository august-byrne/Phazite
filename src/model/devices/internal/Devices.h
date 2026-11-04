// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's own devices (roadmap step 1.6): each one's engine-neutral DSP (src/devices/), which Phazite's engine plays.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"

#include <memory>
#include <span>
#include <string_view>

namespace phazite
{
// Phazite's devices, in the order menus list them.
std::span<const devices::DeviceInfo* const> getOwnDevices();

// One of Phazite's devices by its type ID (DeviceInfo::typeID), or nullptr; and a new one's DSP, for Phazite's engine
// (model/playback/), or nullptr. A type registered as below is found too.
const devices::DeviceInfo* findOwnDevice (std::string_view typeID);
std::unique_ptr<devices::Processor> createOwnDevice (std::string_view typeID);

// Another type of device that songs can hold and Phazite's engine plays as its own (the tests' stand-ins), which no menu
// offers (getOwnDevices() leaves it out). Its description lives as long as the program. Message thread, before a song
// holds one.
using DeviceFactory = std::unique_ptr<devices::Processor> (*)();
void registerDeviceType (const devices::DeviceInfo&, DeviceFactory);
} // namespace phazite
