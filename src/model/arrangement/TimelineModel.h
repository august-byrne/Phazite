// SPDX-License-Identifier: AGPL-3.0-or-later
// The time line as the playlist and the piano roll show it (docs/design.md, sections 6 and 7), in beats from the song's
// start, quarter notes whatever the time signature: its bars, the grid that positions snap to, the playhead and the loop.
// Each function takes the open project's context, and gives what a new song in 4/4 would, or does nothing, while no
// project is open.
#pragma once

#include "GridSettings.h"
#include "model/base/Beats.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <optional>

namespace phazite
{
class ProjectContext;

//==============================================================================
// The song's bars, numbered from 0, as its time signature lays them out from its start: how long the bar that is there is,
// and its time signature's beat, the note it counts (an eighth in 6/8), in quarter notes; the bar a beat is in; and where a
// bar starts.
BeatDuration getBarLength (const ProjectContext&, BeatPosition);
BeatDuration getBeatLength (const ProjectContext&, BeatPosition);
int getBarAt (const ProjectContext&, BeatPosition);
BeatPosition getBarStart (const ProjectContext&, int bar);

//==============================================================================
// The grid (GridSettings.h) over the song's bars. The finest step whose lines are no closer than minLineSpacing at a
// zoom (widthPerBeat, in the same unit: the editors use pixels); and the step that a grid's settings give there, at that
// zoom: the fixed size, or the adaptive one moved by its offset. Where matters only for bars, whose length depends on the
// time signature.
GridStep getAdaptiveGridStep (const ProjectContext&, BeatPosition where, double widthPerBeat, bool triplets, double minLineSpacing);
GridStep getGridStep (const ProjectContext&, const GridSettings&, BeatPosition where, double widthPerBeat, double minLineSpacing);

double getStepInBeats (const ProjectContext&, GridStep, BeatPosition where);
BeatPosition snapToGrid (const ProjectContext&, BeatPosition, GridStep);     // to the nearest line
BeatPosition snapDownToGrid (const ProjectContext&, BeatPosition, GridStep); // to the line at or before it

// Each line of the grid from the start of the range to its end, in order. A line that is both a bar and a beat counts
// as a bar.
void forEachGridLine (const ProjectContext&, GridStep, BeatRange, const std::function<void (BeatPosition, GridLineType)>&);

// The grid an editor keeps in the project (Grid.h): view settings, written without undo steps.
GridSettings loadGridSettings (const ProjectContext&, GridOwner);
void saveGridSettings (ProjectContext&, GridOwner, const GridSettings&);

//==============================================================================
// The playhead: where playback is, or starts from while stopped. Moving it moves where playback starts (Playback.h).
BeatPosition getPlayheadBeat (const ProjectContext&);
void movePlayhead (ProjectContext&, BeatPosition);

// Where the playhead is heard at a moment (milliseconds by juce::Time::getMillisecondCounterHiRes()), which a frame of the
// display shows while the song plays: where playback was as long before as its sound takes to reach the speakers.
BeatPosition getHeardPlayheadBeat (const ProjectContext&, double milliseconds);

// The loop as a ruler shows and changes it: where it is while playback loops (nothing while it doesn't); a range to
// loop, which turns looping on (an empty one changes nothing); looping on or off; and the whole song. Not undo steps,
// as in most DAWs (Playback.h).
std::optional<BeatRange> getLoop (const ProjectContext&);
void loopRange (ProjectContext&, BeatRange);
void toggleLoop (ProjectContext&);
void loopSong (ProjectContext&);
} // namespace phazite
