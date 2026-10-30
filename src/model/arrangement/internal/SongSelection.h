// SPDX-License-Identifier: AGPL-3.0-or-later
// SongSelection's Tracktion side (model/arrangement/SongSelection.h), for the model, the app and tests: one over
// Phazite's selection (model/base/internal/Selection.h), which the model's commands act on. Not for the UI.
#pragma once

#include "model/base/internal/Selection.h"
#include "model/base/internal/Tracktion.h"

#include <memory>

namespace phazite
{
class SongSelection;

// A selection of the items selected in this selection manager, which outlives it.
std::unique_ptr<SongSelection> createSongSelection (Selection&);
} // namespace phazite
