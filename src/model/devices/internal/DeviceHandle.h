// SPDX-License-Identifier: AGPL-3.0-or-later
// Making DeviceHandles (model/devices/DeviceHandle.h) of plug-ins, for the model, the app's device windows and tests.
// Not for the UI, which gets handles from the model's functions by IDs.
#pragma once

#include "model/base/Tracktion.h"

namespace phazite
{
class DeviceHandle;

// Of a plug-in: a handle of nothing unless it is one of Phazite's own devices.
DeviceHandle makeDeviceHandle (te::Plugin&);
} // namespace phazite
