// SPDX-License-Identifier: AGPL-3.0-or-later
// Making MixHandles (model/mixer/MixHandle.h) of plug-ins, for the model and tests. Not for the UI, which gets them by
// IDs (model/mixer/MixerModel.h).
#pragma once

#include "model/base/internal/Tracktion.h"

namespace phazite
{
class MixHandle;

MixHandle makeMixHandle (te::Plugin&);
} // namespace phazite
