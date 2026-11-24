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
// each DEVICE its window's place (DeviceStoreValue.h: writeStoreInto(), writeWindowPlaceInto()); and on each TRACK and the
// MASTER its switches and its view's properties, with its LANES after its clips, its devices and a folder's tracks, before its
// modulators and its sends (TracksArea.h: writeTrackLeavesIn()). The tracks themselves are written by one function
// (WholeState.cpp: writeTracksIn()), into the copy's TRACKS and MASTER, which the song's tree keeps empty: each TRACK in its
// list's order with, in this order, its own properties (TracksValue.h: writeTrackElement()), its switches and its view's
// properties; its clips; its devices and containers; a folder's TRACKs, each written the same; its LANES; its modulators and
// macros; its sends; the MASTER its own properties, its mode and view, its devices, its LANES, its modulators. And on the
// song's root (PHAZITE) the song's own, its properties and its count of IDs (nextID), with its LASTKEY, PIANOROLL and
// MERGING after the root's other children, in that order (SongOwnArea.h: writeSongOwnIn()).
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/SongOwnValue.h"
#include "model/base/internal/SongState.h"
#include "model/base/internal/SongValues.h"
#include "model/devices/ParameterRef.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <optional>

namespace phazite
{
class Song;

// The song's tree, or a part of it (its track list, its TEMPOMAP, any of its root's places), as a copy that holds everything
// the song holds of it: for a song that holds its values, written from its state alone (SongState.h: writeSongTree (getSongState
// (song)), and a place by writeSongPart()). A part that isn't the song's, and any part of a song that keeps everything in its
// tree (one that holds no values: Song::getValues()), is copied as it is. A copy no one else holds, which any thread may then
// read. Message thread.
juce::ValueTree copyWholeSong (const Song&);
juce::ValueTree copyWhole (const Song&, const juce::ValueTree& part);

// A clip of the song by its ID, as a CLIP element that holds everything the song holds of it (an invalid tree for a clip that
// isn't the song's): what reads a clip as a file would hold it (the tests). Message thread.
juce::ValueTree copyWholeClip (const Song&, ClipID);

// The song whole as values at one moment (SongState.h: SongState, getSongState()), kept for what must write or compare the
// song later than it takes it, or on another thread (a save in the background and the copy a project compares itself with:
// model/project/internal/Project.h; a version's song, split on the history's thread; what a frozen track's file holds:
// model/mixer/internal/Freeze.h), with writeSongTree() and readSongTree(), its converters to and from the file's form, which
// build the tree where it is used: the message thread takes the state alone. copyWholeSong() is writeSongTree (getSongState
// (song)) for a song that holds its values.
//
// One device, container or chain of a state, as a DEVICE, CONTAINER or CHAIN element that holds everything the state holds of
// it, as whole state writes it (DevicesArea.h: writeDeviceWhole(), with what the store, the windows' places and modulation
// hold of it and of what is in it written in): what a preset, a copy of a device and the versions' changes take of one
// device. An invalid tree for an ID the state hasn't. Any thread.
juce::ValueTree writeDeviceTree (const SongState&, DeviceID);

// How many whole songs were written as trees from a state since the program started (writeSongTree(): a save's, a version's,
// a test's), and how many of them on the message thread: what the performance tests count (tests/PerformanceGateTests.cpp),
// as the whole tree is built on the thread that uses it (a save's writer, the versions' thread). Any thread.
struct SongTreesWritten
{
    int all = 0, onMessageThread = 0;
};

SongTreesWritten countSongTreesWritten();

// The halves of a comparison of the song with a copy of it (Project's comparisons with its saved copy, the frozen tracks'
// FreezeKeeper), compared as values (the same value, or two alike), or none in either. Any thread. Each area's, defined in its
// area's file (NotesArea.cpp, ClipsArea.cpp, TempoMapArea.cpp), a line each:
//   - whether two kept values hold the same notes and controllers' values for a clip (by its ID);
//   - the same clip (by its ID): its value (its track among it) and its notes;
//   - the same clips on a track (by its ID): the same in the same order, each the same clip.
bool holdSameNotes (const SongState&, const SongState&, ClipID);
bool holdSameClip (const SongState&, const SongState&, ClipID);
bool holdSameTrackClips (const SongState&, const SongState&, TrackID);

// Whether two states hold the same tempo map, as one value (TempoMapValue.h: the same tempos and time signatures, each kind by
// beat): so that a tempo changed and changed back compares as the same; and both a TEMPOMAP among their root's places or
// neither. Both of Project's comparisons with the saved copy compare the tempo map so (the quick one, and the whole song's:
// holdSameSong()), so that the two agree. Any thread. In TempoMapArea.cpp.
bool holdSameTempoMap (const SongState&, const SongState&);

// Whether two states hold the same chord lane: both a lane or neither (ChordLaneValue.h: placed), and the same chords and keys
// as one value (each kind in its order), so that a chord changed and changed back compares as the same. Both of Project's
// comparisons with the saved copy compare the lane so (the quick one, and the whole song's), so that the two agree. Any
// thread. In ChordLaneArea.cpp.
bool holdSameChordLane (const SongState&, const SongState&);

// Whether two states hold the same MIDI links: both a MIDILINKS among their root's places or neither (an empty one is written
// into the file, as the first link made it: SongState::rootPlaces), and the same links as one value (MidiLinksValue.h: in
// their order), so that a link taken away and made again as it was compares as the same. Both of Project's comparisons with
// the saved copy compare the links so (the quick one, and the whole song's), so that the two agree. Any thread. In
// MidiLinksArea.cpp.
bool holdSameMidiLinks (const SongState&, const SongState&);

// Whether two songs' kept values hold the same automation, as values (CurveValues.h: their points' keys aside), so that a
// point moved and moved back, or a curve's points taken out and put back, compares as the same: a parameter's curve (none in
// both, or the same points in both); the same curves' parameters in the same order (the order the song made them in, which
// a curve taken out and made again changes: a difference, as it is in the file); and both (holdSameCurves(): each curve the
// same, in the same order). A song's AUTOMATION is the place of the curves alone, written while it holds one: no matter here.
// Both of Project's comparisons with the saved copy compare the curves so (the quick one, a parameter at a time and their
// order, and the whole song's: holdSameSong()), so that the two agree. In CurvesArea.cpp. Any thread.
bool holdSameCurve (const SongState&, const SongState&, const ParameterRef&);
bool holdSameCurveOrder (const SongState&, const SongState&);
bool holdSameCurves (const SongState&, const SongState&);

// Whether two states hold the same sections: both a SECTIONS among their root's places or neither (an empty one is written
// into the file, as the first section made it), and the same sections as one value (SectionsValue.h: in their order), so that
// a section changed and changed back compares as the same. Both of Project's comparisons with the saved copy compare the
// sections so (the quick one, and the whole song's), so that the two agree. Any thread. In SectionsArea.cpp.
bool holdSameSections (const SongState&, const SongState&);

// Whether two songs' kept values hold the same modulation, as values (ModulationValue.h): all of it; one modulator or macro
// by its ID (none in both, or the same value in both, its place too); and an owner's list (a channel's or a container's:
// ModulatorPlace), the same IDs in the same order, or none in both. Both of Project's comparisons with the saved copy compare
// modulation so (the quick one, a modulator and an owner at a time, and the whole song's, each TRACK's, the MASTER's and each
// CONTAINER's: holdSameSong()), so that the two agree. In ModulationArea.cpp. Any thread.
bool holdSameModulation (const SongState&, const SongState&);
bool holdSameModulator (const SongState&, const SongState&, ModulatorID);
bool holdSameModulatorOrder (const SongState&, const SongState&, const ModulatorPlace& owner);

// Whether two songs' kept values hold the same sends, as values (SendsValue.h): all of them; one send by its ID (none in both,
// or the same value in both, its track too); and a track's list, the same IDs in the same order, or none in both. Both of
// Project's comparisons with the saved copy compare the sends so (the quick one, a send and a track at a time, and the whole
// song's, each track's: holdSameSong()), so that the two agree. In SendsArea.cpp.
// Any thread.
bool holdSameSends (const SongState&, const SongState&);
bool holdSameSend (const SongState&, const SongState&, DeviceID);
bool holdSameTrackSendOrder (const SongState&, const SongState&, TrackID);

// Whether two songs' kept values hold the same of what one device or chain stores (DeviceStoreValue.h: a plug-in's values set
// by hand, its state, program, format ID and version; a chain's mute and solo), the song's entries alone (a held value aside),
// none in both or the same in both. A window's place counts as no change, and is no part of it. Both of Project's comparisons
// with the saved copy compare the store so (the quick one, an item at a time, and the whole song's, each DEVICE's and CHAIN's
// of each device and chain the song holds: holdSameSong()), so that the two agree. In DeviceStoreArea.cpp. Any thread.
bool holdSameDeviceStore (const SongState&, const SongState&, DeviceID);

// Whether two songs' kept values hold the same devices, as values (DevicesValue.h): all of them; one device, container or
// chain by its ID (none in both, or the same value in both, its place too, what the store holds of it aside:
// holdSameDeviceStore()); and an owner's list (a channel's, a container's, a chain's: DevicePlace), the same IDs in the same
// order, or none in both. Both of Project's comparisons with the saved copy compare the devices so (the quick one, an item and
// an owner at a time, and the whole song's, each channel's: holdSameSong()), so that the two agree.
// In DevicesArea.cpp. Any thread.
bool holdSameDevices (const SongState&, const SongState&);
bool holdSameDevice (const SongState&, const SongState&, DeviceID);
bool holdSameDeviceOrder (const SongState&, const SongState&, const DevicePlace& owner);

// Whether two songs' kept values hold the same tracks, as values (TracksValue.h): all of them (each track's value, each list,
// the master's value; the master's count of being put in anew is no part of the song's); one track by its ID, its value (its
// folder among it) and its switches' entries (none in both the same); the master's value and its mode; and one list (a
// folder's, the top list for none), the same IDs in the same order, or none in both. The view counts as no change, and is no
// part of them. Both of Project's comparisons with the saved copy compare the tracks so (the quick one, a track and a list at
// a time, and the whole song's: holdSameSong()), so that the two agree. In TracksArea.cpp. Any thread.
bool holdSameTracks (const SongState&, const SongState&);
bool holdSameTrack (const SongState&, const SongState&, TrackID);
bool holdSameMaster (const SongState&, const SongState&);
bool holdSameTrackList (const SongState&, const SongState&, TrackID folder);

// Whether two states hold the same tracks and what each holds, the master's too: the tracks whole (holdSameTracks(): each
// track's value, each list, the master's value), then each track's clips with their notes (holdSameTrackClips()), its
// modulators and macros (an owner's list and each in it: holdSameModulatorOrder(), holdSameModulator()), its sends
// (holdSameTrackSendOrder(), holdSameSend()), its devices with what each holds and stores (DevicesArea.h:
// holdSameChannelDevices()) and its switches; the master's modulators, devices and switches (holdSameMaster()); and each
// container's macros. Each area compared by identity first (two that share their roots are the same without a look inside),
// then by the IDs the song holds: what an area keeps of a track or a device the song no longer holds (a switch's entry, a
// store's answer) is no part of it, nor is the view (a track's row, a window's place), as a file holds none of them. What
// Project's comparisons with the saved copy ask of the tracks whole (past mostToCompare of them, a store of the plug-ins'
// states: model/project/internal/Project.h). Any thread.
bool holdSameChannels (const SongState&, const SongState&);

// Whether two states hold the same song, as its file would hold it but for what counts as no change: each area in turn, a line
// each, identity first, in this order: the song's own (SongOwnArea.h: holdSameSongOwn(): its own value and its key set last;
// where it stands in its versions, how it plays and shows and its count of IDs never), the tempo map, the chord lane, the
// curves, the MIDI links, the sections (each with whether the song has its place), then the tracks and what they hold
// (holdSameChannels()). The order of the root's places is no part of it, nor the order in which clips joined an alias group (a
// state read from a file has lost it). Project's whole comparison with its saved copy (model/project/internal/Project.h:
// isWholeAsSaved(), checkUnsavedChanges()), which its quick one, area by area, agrees with (tests/SavedCopyTests.cpp). Any
// thread.
bool holdSameSong (const SongState&, const SongState&);
} // namespace phazite
