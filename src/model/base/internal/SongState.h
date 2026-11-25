// SPDX-License-Identifier: AGPL-3.0-or-later
// The song whole, as values, at one moment: every area of the song's values
// (SongValues.h) as it was when it was taken, and the two converters between it and the file's form of the song, a tree of
// elements (Schema.h): writeSongTree(), the tree a whole copy of the song is (WholeState.h: copyWholeSong()), and
// readSongTree(), the areas read from a tree the loader made consistent (model/project/internal/Project.h: makeConsistent()),
// which a song is made of (Song.h: Song (SongState); Project.h: makeSong()).
//
// Any thread may read a state once taken, while the message thread goes on changing the song: each member is a value that
// shares what it holds and changes none of it (immer's containers, juce::String, the store's bytes behind a std::shared_ptr to
// a const block), whose counts of what shares it are atomic (immer's default memory policy: nothing in Immer.h or
// CMakeLists.txt turns its thread safety off, IMMER_NO_THREAD_SAFETY; juce::String's and std::shared_ptr's are atomic too);
// none is a juce::ValueTree, a juce::var that could hold an object, or a pointer into the song.
#pragma once

#include "model/base/internal/ChordLaneValue.h"
#include "model/base/internal/ClipNotesValue.h"
#include "model/base/internal/ClipsValue.h"
#include "model/base/internal/CurveValues.h"
#include "model/base/internal/DeviceStoreValue.h"
#include "model/base/internal/DevicesOutside.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/MidiLinksValue.h"
#include "model/base/internal/ModulationValue.h"
#include "model/base/internal/SectionsValue.h"
#include "model/base/internal/SendsValue.h"
#include "model/base/internal/SongOwnValue.h"
#include "model/base/internal/TempoMapValue.h"
#include "model/base/internal/TrackSwitchesValue.h"
#include "model/base/internal/TracksOutside.h"
#include "model/base/internal/TracksValue.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>

namespace phazite
{
class Song;

// The song whole, as values, at one moment: each area's value (SongValues.h's recipe, a member for each area, which
// getSongState() and readSongTree() take, a line each). What its root holds in a file follows from the values alone: its
// TEMPOMAP, TRACKS and MASTER always; its AUTOMATION while it holds a curve; its MIDILINKS and its SECTIONS while it holds a
// link or a section (an empty list is none); its CHORDLANE while its lane is placed (ChordLaneValue.h: placed).
struct SongState
{
    NotesByClip notes {};
    SongClips clips {};
    TempoMapValue tempoMap {};        // none (no entry of either kind) of a state made empty
    ChordLaneValue chordLane {};      // empty of a song without chords or keys; and whether the song has a lane at all (placed)
    MidiLinksValue midiLinks {};      // none of a song without links
    SongCurves curves {};             // none of a song without automation
    SectionsValue sections {};        // none of a song without sections
    SongModulation modulation {};     // none of a song without modulators or macros
    SongSends sends {};               // none of a song without sends
    DeviceStore deviceStore {};       // none of a song without what devices store
    DevicesOutside devicesOutside {}; // none of a song whose windows never moved
    SongDevices devices {};           // none of a song without devices
    TrackSwitches trackSwitches {};   // none of a song without a switch on
    TracksOutside tracksOutside {};   // none of a song whose rows were never set
    SongTracks tracks {};             // every track and the master, which whole state writes into the TRACKS and the MASTER
    SongOwnValue songOwn {};          // the song's own (SongOwnValue.h)
    SongStanding songStanding {};
    SongOutside songOutside {};
    std::uint64_t nextID = 0; // the song's count of IDs (Song.h: makeID()): 0, none written, for a state made empty
};

// The song whole, as it is now: a copy of each area's value (cheap: a count of what shares it raised for each, never an area
// read or walked: nothing allocated, whatever the song's size: tests/PerformanceGateTests.cpp). Message thread; any thread may
// read it after.
SongState getSongState (const Song&);

// The song's file form, written from a state alone (no song read): a root (PHAZITE) with the song's own properties and its
// count of IDs, and its children in one fixed order, its description's (Schema.h: PHAZITE's elements in the order they are
// described: LASTKEY, TEMPOMAP, PIANOROLL, TRACKS, MASTER, AUTOMATION, MIDILINKS, MERGING, SECTIONS, CHORDLANE), each written
// where the state holds it (SongState, above), each area's elements written into it by the area's own writer (WholeState.h).
// A file whose root's children came in another order is written in this one. A tree no one else holds. Any thread.
juce::ValueTree writeSongTree (const SongState&);

// One of the root's elements written whole from a state, as writeSongTree() writes it (its TRACKS with every track, its
// TEMPOMAP with the map, its SECTIONS with the sections...): an invalid tree for one the state's root doesn't hold (a MIDILINKS
// without links, a CHORDLANE not placed) or one that isn't the root's own place (LASTKEY, PIANOROLL and MERGING are the song's
// own, written by writeSongTree() alone). What asks a part of the song whole that is one of them asks it here (the changes'
// worker, on the history's thread: model/versions/ChangesWorker.cpp, makeSkeleton(); a version's MIDI links and sections
// compared with the song's: model/versions/ApplySong.cpp). Any thread.
juce::ValueTree writeSongPart (const SongState&, const juce::Identifier& type);

// A state read from a song's tree as the loader made it consistent (model/project/internal/Project.h: makeConsistent()), each
// area by its reader as the song holds it (the areas' readHeld<Area>In(), a line each, in the order of the areas, the song's
// own first: each element read once, and no tree changed), and its curves' points given keys from 1 (CurveValues.h:
// PointValue; never saved nor compared). What a song is made of as it opens (Project.h: makeSong()). Any thread.
SongState readSongTree (const juce::ValueTree& consistent);
} // namespace phazite
