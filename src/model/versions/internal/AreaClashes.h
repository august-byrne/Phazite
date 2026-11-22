// SPDX-License-Identifier: AGPL-3.0-or-later
// A merge's clash chosen in an area the song holds as a value beside its tree (roadmap step S5.20; model/base/internal/
// SongValues.h), which a write of the song's tree can't reach: each such area's chooser, declared here and defined with its
// area's model code, which chooseClash() (model/versions/VersionsModel.cpp: chooseInAreas()) asks in turn, a line each,
// before it writes the song's tree. A new area adds its chooser here and its line there (SongValues.h's recipe). Message
// thread: each sets its area through the area's code, in the choice's undo step.
#pragma once

#include "model/versions/internal/SongMerge.h"

#include <juce_data_structures/juce_data_structures.h>

namespace phazite
{
class Song;

// What an area's chooser did with a clash: nothing (the clash's path isn't the area's: the next area is asked), chose it (the
// song changed, in the choice's step), or refused it (nothing written: the choice stays as it was, and no other area nor the
// song's tree is asked).
enum class AreaChoice
{
    notTheArea,
    chosen,
    refused
};

// The three songs whole that a clash's three values come from: the parting's, this line's before the merge, the other line's
// (VersionHistory.h: Merge::base, before, theirs).
struct MergeSides
{
    juce::ValueTree base, before, theirs;
};

// The areas' choosers, each with the clash and the value chosen (0 what was before both, 1 this line's, 2 the other line's):
//   - the tempo map's (model/transport/Tempo.cpp): a path into the song's TEMPOMAP, through the tempo map's value;
//   - the chord lane's (model/chords/Chords.cpp): a path into the song's CHORDLANE, which merges whole, through the chord
//     lane's code (its element and its value);
//   - a note's (model/notes/Notes.cpp): a note's property (a path that ends at a NOTE, in a clip), through the notes' code;
//   - a clip's (model/arrangement/Clips.cpp): a clip's own property (a path that ends at a clip on the track the path names),
//     through the clips' code; and any other clash whose path goes through a clip, refused (nothing else inside a clip has a
//     property or an element a merge reports: its markers and controllers' values merge as lists).
// A note's path goes through its clip: its chooser is asked before the clip's.
AreaChoice chooseTempoMapClash (Song&, const MergeClash&, int value, const MergeSides&);
AreaChoice chooseChordLaneClash (Song&, const MergeClash&, int value, const MergeSides&);
AreaChoice chooseNoteClash (Song&, const MergeClash&, int value, const MergeSides&);
AreaChoice chooseClipClash (Song&, const MergeClash&, int value, const MergeSides&);
} // namespace phazite
