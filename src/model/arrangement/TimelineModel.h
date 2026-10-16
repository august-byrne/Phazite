// SPDX-License-Identifier: AGPL-3.0-or-later
// The time line as the playlist and the piano roll show it (docs/design.md, sections 6 and 7), in beats from the song's
// start (architecture phase A3: no Tracktion here): its bars, the grid that positions snap to, the playhead and the
// loop. Each function takes the open project's context, and gives what a new song in 4/4 would, or does nothing, while
// no project is open.
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
// The song's bars, numbered from 0, as its time signature lays them out from its start.
int getBeatsPerBar (const ProjectContext&, BeatPosition); // in the bar that is there
int getBarAt (const ProjectContext&, BeatPosition);
BeatPosition getBarStart (const ProjectContext&, int bar);

//==============================================================================
// A size of the grid: one of the bars-and-beats levels (GridSettings.h: 7 is 1/4 beat, 9 a beat, 10 a bar), straight or
// in triplets.
struct GridStep
{
    int level = 9;
    bool triplets = false;

    bool operator== (const GridStep&) const = default;
};

// The coarsest step whose lines are no closer than minLineSpacing at a zoom (widthPerBeat, in the same unit: the editors
// use pixels); and the step that a grid's settings give there, at that zoom: the fixed size, or the adaptive one moved
// by its offset. Where matters only for bars, whose length depends on the time signature.
GridStep getAdaptiveGridStep (const ProjectContext&, BeatPosition where, double widthPerBeat, bool triplets, double minLineSpacing);
GridStep getGridStep (const ProjectContext&, const GridSettings&, BeatPosition where, double widthPerBeat, double minLineSpacing);

double getStepInBeats (const ProjectContext&, GridStep, BeatPosition where);
BeatPosition snapToGrid (const ProjectContext&, BeatPosition, GridStep);     // to the nearest line
BeatPosition snapDownToGrid (const ProjectContext&, BeatPosition, GridStep); // to the line at or before it
juce::String describeGridStep (const ProjectContext&, GridStep);             // "1/4 beat", "Bar"

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

// The loop as a ruler shows and changes it: where it is while playback loops (nothing while it doesn't); a range to
// loop, which turns looping on (an empty one changes nothing); looping on or off; and the whole song. Not undo steps,
// as in most DAWs (Playback.h).
std::optional<BeatRange> getLoop (const ProjectContext&);
void loopRange (ProjectContext&, BeatRange);
void toggleLoop (ProjectContext&);
void loopSong (ProjectContext&);
} // namespace phazite
