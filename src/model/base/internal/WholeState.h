// SPDX-License-Identifier: AGPL-3.0-or-later
// The song whole, or a part of it whole: what needs the song or a part of it with everything it holds (a save, a version's
// pieces, a comparison, a merge, the clipboard, a duplicate, an export) asks these, never the song's tree alone
// (Song::getState().createCopy()). The song keeps areas of itself as values beside its tree (roadmap step S5.20;
// model/base/internal/SongValues.h): its clips, a MIDI clip's notes and its controllers' values, and its tempo map, which its
// tree doesn't hold. These write what is kept beside the tree back in, so that what asks gets the tree a song that kept
// everything in its tree would give: under each track, its clips first among its elements, in the track's order, each
// written as ClipsValue.h's writeClipElement() writes it (each property only while it is set; an audio clip's warp markers;
// a MIDI clip's NOTE elements, then its CONTROL elements, each kind in its order); in the song's TEMPOMAP element, which
// the tree keeps empty for it, its tempos then its time signatures, each kind by beat, each written as TempoMapValue.h's
// writeTempoMap() writes it (a tempo's curve only while it isn't 1); in the song's CHORDLANE, where it has one, its
// chords then its key changes, each kind in its order, each written as ChordLaneValue.h's writeChordLane() writes it; and in
// the song's MIDILINKS, where it has one, its MIDI links in their order, each written as MidiLinksValue.h's writeMidiLinks()
// writes it; and in the song's AUTOMATION, while it has a curve (a song without one is written without an AUTOMATION), its
// curves in their order, each written as CurveValues.h's writeCurves() writes it; under each TRACK, the MASTER and each
// CONTAINER, its modulators and macros in their order, after its other children, each written as ModulationValue.h's
// writeModulatorElement() writes it; and under each TRACK, its sends in their order, after those (the last of its children),
// each written as SendsValue.h's writeSendElement() writes it; and in each plug-in's DEVICE what the store holds of it, its
// values set by hand as VALUEs sorted by their parameters after its other children, in each CHAIN its mute and solo, and in
// each DEVICE its window's place (DeviceStoreValue.h: writeStoreInto(), writeWindowPlaceInto()).
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/SongValues.h"
#include "model/devices/ParameterRef.h"

#include <juce_data_structures/juce_data_structures.h>

namespace phazite
{
class Song;

// The song's tree, or a part of it (a TRACK, the track list, the TEMPOMAP, any element of the song), as a copy that holds
// everything the song holds of it: what is kept beside the tree written back in (each track's clips, with their notes; the
// tempo map, in the song's or its TEMPOMAP's copy). A part that isn't
// the song's, and any part of a song that keeps everything in its tree (one that holds no values: Song::getValues()), is
// copied as it is. A copy no one else holds, which any thread may then read. Message thread.
juce::ValueTree copyWholeSong (const Song&);
juce::ValueTree copyWhole (const Song&, const juce::ValueTree& part);

// A clip of the song by its ID, as a CLIP element that holds everything the song holds of it (an invalid tree for a clip that
// isn't the song's): what reads a clip as a file would hold it (the tests). Message thread.
juce::ValueTree copyWholeClip (const Song&, ClipID);

// What the song keeps beside its tree, as it is now: kept for nothing (values share what they hold), for what must write or
// compare the song later than it copies it (a save in the background, the copy a project compares itself with: model/
// project/internal/Project.h; what a frozen track's file holds: model/mixer/internal/Freeze.h). Together with a copy of the
// tree taken at the same moment it gives the whole state then. Any thread may read one once kept. A member for each area
// (SongValues.h's recipe), which keepValues() keeps, a line each.
struct ValuesKept
{
    NotesByClip notes;
    SongClips clips;
    TempoMapValue tempoMap;           // none (no entry of either kind) kept of a song that holds no values
    ChordLaneValue chordLane;         // empty kept of a song that holds no values, and of one without chords or keys
    MidiLinksValue midiLinks;         // none kept of a song that holds no values, and of one without links
    SongCurves curves;                // none kept of a song that holds no values, and of one without automation
    SectionsValue sections;           // none kept of a song that holds no values, and of one without sections
    SongModulation modulation {};     // none kept of a song that holds no values, and of one without modulators or macros
    SongSends sends {};               // none kept of a song that holds no values, and of one without sends
    DeviceStore deviceStore {};       // none kept of a song that holds no values, and of one without what devices store
    DevicesOutside devicesOutside {}; // none kept of a song that holds no values, and of one whose windows never moved
};

ValuesKept keepValues (const Song&); // message thread

// A copy of the song's tree (or of a part of it) taken with keepValues() at the same moment, made whole: a copy of it with
// the kept values written in, which the tree given doesn't get. Any thread.
juce::ValueTree copyWhole (const juce::ValueTree& treeCopy, const ValuesKept&);

// The halves of a comparison of the song with a copy of it that the trees don't hold (Project's holdTheSameProject(), the
// frozen tracks' FreezeKeeper), compared as values (the same value, or two alike), or none in either. Any thread. Each
// area's, defined in its area's file (NotesArea.cpp, ClipsArea.cpp, TempoMapArea.cpp), a line each:
//   - whether two kept values hold the same notes and controllers' values for a clip (by its ID);
//   - the same clip (by its ID): its value (its track among it) and its notes;
//   - the same clips on a track (by its ID): the same in the same order, each the same clip.
bool holdSameNotes (const ValuesKept&, const ValuesKept&, ClipID);
bool holdSameClip (const ValuesKept&, const ValuesKept&, ClipID);
bool holdSameTrackClips (const ValuesKept&, const ValuesKept&, TrackID);

// Whether two songs, each a tree with its kept values (the song's own and its kept values now, a copy and those kept with it),
// hold the same tempo map, as one value (TempoMapValue.h: the same tempos and time signatures, each kind by beat): so that a
// tempo changed and changed back compares as the same. The values kept are compared, the trees' TEMPOMAP elements being the
// places of the maps alone: a tree with one and a tree without hold different maps. Both of Project's comparisons with the
// saved copy compare the tempo map with it (the quick one, and the whole song's: holdTheSameProject()), so that the two
// agree. Any thread.
bool holdSameTempoMap (const juce::ValueTree& song, const ValuesKept&, const juce::ValueTree& otherSong, const ValuesKept&);

// Whether two songs, each a tree with its kept values, hold the same chord lane: both a lane or neither (the trees' CHORDLANE
// elements, the sign of one), and the same chords and keys as one value (ChordLaneValue.h: each kind in its order), so that
// a chord changed and changed back compares as the same. Both of Project's comparisons with the saved copy compare the lane
// so (the quick one, and the whole song's), so that the two agree. Any thread. In ChordLaneArea.cpp.
bool holdSameChordLane (const juce::ValueTree& song, const ValuesKept&, const juce::ValueTree& otherSong, const ValuesKept&);

// Whether two songs, each a tree with its kept values, hold the same MIDI links: both a MIDILINKS or neither (the trees'
// elements: an empty one is written into the file, as the first link made it), and the same links as one value
// (MidiLinksValue.h: in their order), so that a link taken away and made again as it was compares as the same. Both of
// Project's comparisons with the saved copy compare the links so (the quick one, and the whole song's), so that the two agree.
// Any thread. In MidiLinksArea.cpp.
bool holdSameMidiLinks (const juce::ValueTree& song, const ValuesKept&, const juce::ValueTree& otherSong, const ValuesKept&);

// Whether two songs' kept values hold the same automation, as values (CurveValues.h: their points' keys aside), so that a
// point moved and moved back, or a curve's points taken out and put back, compares as the same: a parameter's curve (none in
// both, or the same points in both); the same curves' parameters in the same order (the order the song made them in, which
// a curve taken out and made again changes: a difference, as it is in the file); and both (holdSameCurves(): each curve the
// same, in the same order). The trees' AUTOMATION elements are the places of the curves alone, which every song that holds
// its values has: no matter here. Both of Project's comparisons with the saved copy compare the curves so (the quick one, a
// parameter at a time and their order, and the whole song's: holdTheSameProject()), so that the two agree. In
// CurvesArea.cpp. Any thread.
bool holdSameCurve (const ValuesKept&, const ValuesKept&, const ParameterRef&);
bool holdSameCurveOrder (const ValuesKept&, const ValuesKept&);
bool holdSameCurves (const ValuesKept&, const ValuesKept&);

// Whether two songs, each a tree with its kept values, hold the same sections: both a SECTIONS or neither (the trees'
// elements: an empty one is written into the file, as the first section made it), and the same sections as one value
// (SectionsValue.h: in their order), so that a section changed and changed back compares as the same. Both of Project's
// comparisons with the saved copy compare the sections so (the quick one, and the whole song's), so that the two agree. Any
// thread. In SectionsArea.cpp.
bool holdSameSections (const juce::ValueTree& song, const ValuesKept&, const juce::ValueTree& otherSong, const ValuesKept&);

// Whether two songs' kept values hold the same modulation, as values (ModulationValue.h): all of it; one modulator or macro
// by its ID (none in both, or the same value in both, its place too); and an owner's list (a channel's or a container's:
// ModulatorPlace), the same IDs in the same order, or none in both. Both of Project's comparisons with the saved copy compare
// modulation so (the quick one, a modulator and an owner at a time, and the whole song's, each TRACK's, the MASTER's and each
// CONTAINER's as holdSameKeptOf() asks them), so that the two agree. The trees hold none of it. In ModulationArea.cpp. Any
// thread.
bool holdSameModulation (const ValuesKept&, const ValuesKept&);
bool holdSameModulator (const ValuesKept&, const ValuesKept&, ModulatorID);
bool holdSameModulatorOrder (const ValuesKept&, const ValuesKept&, const ModulatorPlace& owner);

// Whether two songs' kept values hold the same sends, as values (SendsValue.h): all of them; one send by its ID (none in both,
// or the same value in both, its track too); and a track's list, the same IDs in the same order, or none in both. Both of
// Project's comparisons with the saved copy compare the sends so (the quick one, a send and a track at a time, and the whole
// song's, each TRACK's as holdSameKeptOf() asks it), so that the two agree. The trees hold none of them. In SendsArea.cpp.
// Any thread.
bool holdSameSends (const ValuesKept&, const ValuesKept&);
bool holdSameSend (const ValuesKept&, const ValuesKept&, DeviceID);
bool holdSameTrackSendOrder (const ValuesKept&, const ValuesKept&, TrackID);

// Whether two songs' kept values hold the same of what one device or chain stores (DeviceStoreValue.h: a plug-in's values set
// by hand, its state, program, format ID and version; a chain's mute and solo), the song's entries alone (a held value aside),
// none in both or the same in both. A window's place counts as no change, and is no part of it. Both of Project's comparisons
// with the saved copy compare the store so (the quick one, an item at a time, and the whole song's, each DEVICE's and CHAIN's
// as holdSameKeptOf() asks it), so that the two agree. The trees hold none of it. In DeviceStoreArea.cpp. Any thread.
bool holdSameDeviceStore (const ValuesKept&, const ValuesKept&, DeviceID);

// Whether two elements of the same type (a song's tree or a part of it, each with its song's kept values) hold the same of
// what the areas keep beside the tree for them: each area asked in turn, a line each, in this order: a track's clips
// (holdSameTrackClips()), then the tempo map of an element either of which holds a TEMPOMAP (holdSameTempoMap()), then the
// chord lane of a song's element either of which holds a CHORDLANE (holdSameChordLane()), then the curves of two songs'
// elements (holdSameCurves()), then the MIDI links of a song's element either of which holds a MIDILINKS
// (holdSameMidiLinks()), then the sections of a song's element either of which holds a SECTIONS (holdSameSections()), then
// a TRACK's, the MASTER's or a CONTAINER's modulators and macros (ModulationArea.h: holdSameModulationOf()), then a TRACK's
// sends (SendsArea.h: holdSameSendsOf()), then what a DEVICE or a CHAIN stores (DeviceStoreArea.h: holdSameDeviceStoreOf()). What
// Project's comparison of an element whole asks once the element's own properties compare (holdTheSameProject()). And
// whether a child of an element is the place of what an area keeps (the song's TEMPOMAP, its CHORDLANE, its MIDILINKS, its
// AUTOMATION, its SECTIONS), which that comparison leaves out of the elements it compares one by one. Any thread.
bool holdSameKeptOf (const juce::ValueTree& element, const ValuesKept&, const juce::ValueTree& otherElement, const ValuesKept&);
bool isPlaceOfKept (const juce::ValueTree& parent, const juce::ValueTree& child);
} // namespace phazite
