// SPDX-License-Identifier: AGPL-3.0-or-later
// Making LevelSources (model/mixer/LevelSource.h) of Tracktion's level measurers, for the model and tests. Not for the
// UI, which gets them by IDs (model/mixer/MixerModel.h).
#pragma once

#include "model/base/internal/Tracktion.h"

#include <functional>

namespace phazite
{
class LevelSource;

// A source that reads whichever measurer findMeasurer gives now (nullptr for none), as a client of it.
LevelSource makeLevelSource (std::function<te::LevelMeasurer*()> findMeasurer);
} // namespace phazite
