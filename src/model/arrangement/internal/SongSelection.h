// SPDX-License-Identifier: AGPL-3.0-or-later
// SongSelection's Tracktion side (model/arrangement/SongSelection.h), for the model, the app and tests: one over
// Tracktion's selection manager, which the model's commands act on. Not for the UI.
#pragma once

#include "model/base/internal/Tracktion.h"

#include <memory>

namespace phazite
{
class SongSelection;

// A selection of the items selected in this selection manager, which outlives it.
std::unique_ptr<SongSelection> createSongSelection (te::SelectionManager&);
} // namespace phazite
