// SPDX-License-Identifier: AGPL-3.0-or-later
// Making the SongRange (model/arrangement/SongRange.h) of a range selection, for the project's context and tests.
#pragma once

#include "model/arrangement/internal/Selection.h"

#include <memory>

namespace phazite
{
class SongRange;

// A view of the range, finding tracks in the selection manager's Edit.
std::unique_ptr<SongRange> createSongRange (RangeSelection&, const te::SelectionManager&);
} // namespace phazite
