// SPDX-License-Identifier: AGPL-3.0-or-later
// Making the SongRange (model/arrangement/SongRange.h) of a range selection, for the project's context and tests.
#pragma once

#include "model/arrangement/internal/Selection.h"
#include "model/base/internal/Selection.h"

#include <memory>

namespace phazite
{
class SongRange;

// A view of the range, finding its tracks in the selection's song.
std::unique_ptr<SongRange> createSongRange (RangeSelection&, const Selection&);
} // namespace phazite
