// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's own devices (roadmap step 1.6): each one's engine-neutral DSP (src/devices/) in a DevicePlugin.
#pragma once

#include "model/base/Tracktion.h"

namespace phazite
{
// Registers each of Phazite's devices with the engine, so that projects can hold them. createEngine() calls it.
void registerDevices (te::Engine&);
} // namespace phazite
