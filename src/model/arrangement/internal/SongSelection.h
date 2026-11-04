// SPDX-License-Identifier: AGPL-3.0-or-later
// Making the SongSelection (model/arrangement/SongSelection.h), for the model, the app and tests: one over
// Phazite's selection (model/base/internal/Selection.h), which the model's commands act on. Not for the UI.
#pragma once

#include "model/base/internal/Selection.h"

#include <memory>

namespace phazite
{
class SongSelection;

// A view of the items selected in this selection, which outlives it.
std::unique_ptr<SongSelection> createSongSelection (Selection&);
} // namespace phazite
