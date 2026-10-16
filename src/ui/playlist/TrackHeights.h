// SPDX-License-Identifier: AGPL-3.0-or-later
// How tall the playlist shows tracks, and the commands that make them taller and shorter. The project keeps each
// track's height as saved (TrackRow::savedHeight), with whether it is collapsed.
#pragma once

#include "model/arrangement/PlaylistModel.h"

#include <vector>

namespace phazite
{
class CommandRegistry;
} // namespace phazite

namespace phazite::ui
{
// Track heights in the playlist, at the normal interface size.
namespace TrackHeight
{
    inline constexpr int minimum = 32, standard = 64, maximum = 320, step = 16;
    inline constexpr int collapsed = 24; // a collapsed track's row, which shows its name
} // namespace TrackHeight

int getTrackHeight (const TrackRow&); // the saved height, between the minimum and the maximum

// How tall the playlist shows a track: its height, or TrackHeight::collapsed while it is collapsed. A collapsed
// folder keeps its height, as collapsing it hides its tracks instead.
int getShownTrackHeight (const TrackRow&);

// Sets how tall the playlist shows a track, as dragging its bottom edge does: well below the minimum, a track
// collapses, and above it, a collapsed track expands again. (A folder stops at the minimum.) View state, without undo
// steps.
void setShownTrackHeight (ProjectContext&, TrackID, int height);

// Makes tracks taller or shorter by some pixels, as the height commands do, collapsing or expanding them the same way.
void resizeTracks (ProjectContext&, const std::vector<TrackID>&, int change);

// Taller Tracks and Shorter Tracks, for the selected tracks, or all tracks if none is selected. The app registers them
// right after the model's track commands (addTrackCommands()), which decides their place in the command order.
void addTrackViewCommands (CommandRegistry&, ProjectContext&);
} // namespace phazite::ui
