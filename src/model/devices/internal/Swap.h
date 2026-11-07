// SPDX-License-Identifier: AGPL-3.0-or-later
// The swaps open on a song's devices (model/devices/SwapModel.h), by the song: for device handles, whose editors light the
// pad being swapped. Not for the UI.
#pragma once

#include "model/base/IDs.h"
#include "model/base/Song.h"

#include <optional>

namespace phazite
{
// The swap open on a device of the song, if one is: the pad it swaps, or -1 for the device's presets.
std::optional<int> findOpenSwap (const Song&, DeviceID);
} // namespace phazite
