// SPDX-License-Identifier: AGPL-3.0-or-later
// The grids the editors keep in the song (GridSettings.h has the grid itself; the editors reach it through
// TimelineModel.h).
#pragma once

#include "model/arrangement/GridSettings.h"
#include "model/base/Song.h"

namespace phazite
{
struct SavedGrid; // model/base/internal/SongOwnValue.h

GridSettings loadGridSettings (const Song&, GridOwner); // the defaults if there are none; reading adds nothing
void saveGridSettings (Song&, GridOwner, const GridSettings&);

// A grid as the song holds it (SongOwnValue.h: SavedGrid, what a tree of the project's state is read as too: readSavedGrid(),
// GridSettings::load()): its offset within the grid's, its step's level where it is one of the saved words, else the default.
GridSettings loadGridSettings (const SavedGrid&);
} // namespace phazite
