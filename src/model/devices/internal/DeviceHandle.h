// SPDX-License-Identifier: AGPL-3.0-or-later
// Making DeviceHandles (model/devices/DeviceHandle.h) of a song's devices, for the model, the app's device windows and
// tests. Not for the UI, which gets handles from the model's functions by IDs.
#pragma once

#include "model/base/IDs.h"
#include "model/base/Song.h"

namespace phazite
{
class DeviceHandle;

// Of a device of the song: a handle of nothing unless it is one of Phazite's own devices (or of a type registered as one:
// internal/Devices.h).
DeviceHandle makeDeviceHandle (Song&, DeviceID);
} // namespace phazite
