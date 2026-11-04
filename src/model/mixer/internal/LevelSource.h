// SPDX-License-Identifier: AGPL-3.0-or-later
// Making LevelSources (model/mixer/LevelSource.h) of Phazite's engine's meters, for the model and tests. Not for the UI,
// which gets them by IDs (model/mixer/MixerModel.h).
#pragma once

#include "engine/nodes/LevelMeter.h"

#include <functional>

namespace phazite
{
class LevelSource;

// A source that reads whichever meter find gives now (nullptr for none): a channel's, while Phazite's engine plays it.
LevelSource makeLevelSource (std::function<engine::LevelMeter*()> find);
} // namespace phazite
