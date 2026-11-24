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

// An editor's grid saved in the song, as view state: outside the history, no change of the song's (SongOwnValue.h:
// setSongOutside(), the song's SongOutside::playlistGrid or pianoRollGrid).
void saveGridSettings (Song&, GridOwner, const GridSettings&);

// A grid as the song holds it (SongOwnValue.h: SavedGrid, what a tree of a file's grid is read as too: readSavedGrid()): its
// offset within the grid's, its step's level where it is one of the saved words, else the default. And a grid as the song
// saves it: each of its settings, the fixed size by its saved word (getSavedGridStep()).
GridSettings loadGridSettings (const SavedGrid&);
SavedGrid makeSavedGrid (const GridSettings&);
} // namespace phazite
