// SPDX-License-Identifier: AGPL-3.0-or-later
// The grids the editors keep in the song (GridSettings.h has the grid itself; the editors reach it through
// TimelineModel.h).
#pragma once

#include "model/arrangement/GridSettings.h"
#include "model/base/Song.h"

namespace phazite
{
GridSettings loadGridSettings (const Song&, GridOwner); // the defaults if there are none; reading adds nothing
void saveGridSettings (Song&, GridOwner, const GridSettings&);
} // namespace phazite
