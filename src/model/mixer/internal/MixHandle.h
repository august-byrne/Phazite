// SPDX-License-Identifier: AGPL-3.0-or-later
// Making MixHandles (model/mixer/MixHandle.h) of a song's devices, for the model and tests. Not for the UI, which gets them
// by IDs (model/mixer/MixerModel.h).
#pragma once

#include "model/base/IDs.h"
#include "model/base/Song.h"

namespace phazite
{
class MixHandle;

// Of a device of the song, while the song lives and holds it.
MixHandle makeMixHandle (Song&, DeviceID);
} // namespace phazite
