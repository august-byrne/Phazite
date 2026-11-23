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
//   - a MIDI link's (model/midi/MidiLinks.cpp): a path to a MIDILINK in the song's MIDILINKS, which merges whole, through
//     the links' code;
//   - a curve's (model/automation/Automation.cpp): a path to a CURVE of the song's AUTOMATION, which merges whole, through
//     automation's functions by the curve's parameter; any other clash through a curve, refused;
//   - a note's (model/notes/Notes.cpp): a note's property (a path that ends at a NOTE, in a clip), through the notes' code;
//   - a clip's (model/arrangement/Clips.cpp): a clip's own property (a path that ends at a clip on the track the path names),
//     through the clips' code; and any other clash whose path goes through a clip, refused (nothing else inside a clip has a
//     property or an element a merge reports: its markers and controllers' values merge as lists).
// A note's path goes through its clip: its chooser is asked before the clip's.
AreaChoice chooseTempoMapClash (Song&, const MergeClash&, int value, const MergeSides&);
AreaChoice chooseChordLaneClash (Song&, const MergeClash&, int value, const MergeSides&);

// The MIDI links' (model/midi/MidiLinks.cpp): a path into the song's MIDILINKS, whose links merge whole, each by its
// controller, through the MIDI links' value.
AreaChoice chooseMidiLinkClash (Song&, const MergeClash&, int value, const MergeSides&);

// The sections' (model/arrangement/Sections.cpp): a path into the song's SECTIONS, whose sections merge by their IDs: a
// section's property, or a whole section, through the sections' value.
AreaChoice chooseSectionClash (Song&, const MergeClash&, int value, const MergeSides&);

// Modulation's (model/modulation/Modulation.cpp): a path through a MODULATOR or a MACRO of a channel or a container, which
// merge as items by their IDs: a property of one, one whole, or a property of what is in one (a modulation's amount, a
// value set by hand) or what is in one whole, through modulation's setters by the modulator's ID; a modulation both lines
// made, whose amount's clash has no value before, taken away for what was before (known bug 239's shape).
AreaChoice chooseModulationClash (Song&, const MergeClash&, int value, const MergeSides&);

// The sends' (model/mixer/Mixer.cpp): a path through a SEND of a track, which merge as items by their IDs: a property of one,
// or one whole, through the sends' setters by the send's ID; a send both lines made under one ID, whose property's clash the
// side chosen holds none of, taken away (known bug 239's shape).
AreaChoice chooseSendClash (Song&, const MergeClash&, int value, const MergeSides&);

// The devices' (model/mixer/DeviceClashes.cpp): a path through a DEVICE, a CONTAINER or a CHAIN (its last such step names
// the item, the steps before it its place), which merge as items by their IDs, each property at a time. What the store holds
// of one (a plug-in's state, program, format ID or version, a value of its set by hand, a chain's mute or solo) through the
// store's functions (model/plugins/internal/DeviceStore.h), its own chooser asked first; a property of the item's own, of its
// VALUES or of its SOUNDS through the devices' setter (model/mixer/internal/Mixer.h: setDevice()). A plug-in's value both
// lines set of a parameter the parting's song held none of, chosen as what was before, taken away (known bug 239's shape).
// Any other path through one (an item whole, which a merge never reports for these, or what is deeper in one) refused.
AreaChoice chooseDeviceStoreClash (Song&, const MergeClash&, int value, const MergeSides&);
AreaChoice chooseDeviceClash (Song&, const MergeClash&, int value, const MergeSides&);

AreaChoice chooseCurveClash (Song&, const MergeClash&, int value, const MergeSides&);
AreaChoice chooseNoteClash (Song&, const MergeClash&, int value, const MergeSides&);
AreaChoice chooseClipClash (Song&, const MergeClash&, int value, const MergeSides&);
} // namespace phazite
