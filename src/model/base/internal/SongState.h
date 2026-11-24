// SPDX-License-Identifier: AGPL-3.0-or-later
// The song whole, as values, at one moment (roadmap step S5.20, its end: DESIGN's SongState): every area of the song's values
// (SongValues.h) as it was when it was taken, and the two converters between it and the file's form of the song, a tree of
// elements (Schema.h): writeSongTree(), the tree a whole copy of the song is (WholeState.h: copyWholeSong()), and
// readSongTree(), the areas read from a tree the loader made consistent (model/project/internal/Project.h: makeConsistent()),
// as the song reads its file as it holds its values (Song::holdValues()).
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

#include <array>
#include <cstdint>
#include <optional>

namespace phazite
{
class Song;

// A place the song's root holds for an area: an empty element of its own in the song's tree (SongValues.h: its TEMPOMAP, its
// AUTOMATION, its TRACKS and its MASTER always; its CHORDLANE, its MIDILINKS and its SECTIONS where it has them), which a whole
// copy writes the area into. None, for the end of a state's list of them.
enum class RootPlace : std::uint8_t
{
    none,
    tempoMap,
    tracks,
    master,
    automation,
    midiLinks,
    sections,
    chordLane
};

// A place's element type (Schema.h), and the place an element type is (none for any other: the root's LASTKEY, PIANOROLL and
// MERGING are the song's own, which whole state writes after them).
const juce::Identifier& getRootPlaceType (RootPlace);
RootPlace findRootPlace (const juce::Identifier& type);

// The song whole, as values, at one moment: each area's value (SongValues.h's recipe, a member for each area, which
// getSongState() and readSongTree() take, a line each) and the places its root holds, in the root's order.
struct SongState
{
    NotesByClip notes {};
    SongClips clips {};
    TempoMapValue tempoMap {};        // none (no entry of either kind) of a song that holds no values
    ChordLaneValue chordLane {};      // empty of a song that holds no values, and of one without chords or keys; and
                                      // whether the song has a lane at all (ChordLaneValue.h: placed)
    MidiLinksValue midiLinks {};      // none of a song that holds no values, and of one without links
    SongCurves curves {};             // none of a song that holds no values, and of one without automation
    SectionsValue sections {};        // none of a song that holds no values, and of one without sections
    SongModulation modulation {};     // none of a song that holds no values, and of one without modulators or macros
    SongSends sends {};               // none of a song that holds no values, and of one without sends
    DeviceStore deviceStore {};       // none of a song that holds no values, and of one without what devices store
    DevicesOutside devicesOutside {}; // none of a song that holds no values, and of one whose windows never moved
    SongDevices devices {};           // none of a song that holds no values, and of one without devices
    TrackSwitches trackSwitches {};   // none of a song that holds no values, and of one without a switch on
    TracksOutside tracksOutside {};   // none of a song that holds no values, and of one whose rows were never set

    // None of a song that holds no values (its tree holds its tracks); else every track and the master, which whole state
    // writes into the TRACKS and the MASTER (WholeState.cpp: writeTracksIn()).
    std::optional<SongTracks> tracks {};

    SongOwnValue songOwn {}; // the song's own (SongOwnValue.h): the defaults of a song that holds no values
    SongStanding songStanding {};
    SongOutside songOutside {};
    std::uint64_t nextID = 0; // the song's count of IDs (Song.h: makeID()): 0, none written, for a song that holds no values

    // The places its root holds, in the root's order, the first none ending them (a place once each): those of the song's
    // tree as it was taken (getSongState()), or of the tree read (readSongTree()), so that writeSongTree() writes the root's
    // children in the order the song's tree holds them, the elements of its own after them. Until the song's tree goes (the
    // end of roadmap step S5.20, which writes them in its description's order) the order of a song's root is its history's:
    // a CHORDLANE, a MIDILINKS or a SECTIONS added where the song's first chord, link or section added it. Whether the song
    // has a MIDILINKS or a SECTIONS at all is said here alone (each, added with the first link or section, stays once the last
    // goes); whether it has a CHORDLANE, its value's too (ChordLaneValue.h: placed), which a song keeps equal to this
    // (tests/SongCheck.cpp). Those every song that holds its values has, as a new state starts.
    std::array<RootPlace, 7> rootPlaces { RootPlace::tempoMap, RootPlace::tracks, RootPlace::master, RootPlace::automation };

    // Whether the root holds a place (rootPlaces).
    bool holdsPlace (RootPlace) const noexcept;
};

// The song whole, as it is now: a copy of each area's value (cheap: a count of what shares it raised for each, never an area
// read or walked: nothing allocated, whatever the song's size: tests/PerformanceGateTests.cpp) and its root's places. For a
// song that holds no values, the defaults (and the places its tree holds). Message thread; any thread may read it after.
SongState getSongState (const Song&);

// The song's file form, written from a state alone (no song read): a root (PHAZITE) with the places the state names, in its
// order (its CHORDLANE only where its lane is placed; its TEMPOMAP where the state names none, and its AUTOMATION where it holds
// curves, after the others), each area's elements written into it by the area's own writer as whole state writes them
// (WholeState.h: the song's own's properties, its count of IDs, then its LASTKEY, PIANOROLL and MERGING after the places; its
// AUTOMATION left out while it holds no curve), exactly what a whole copy of the song gave while its tree was the song's
// storage (copyWholeSong()). A tree no one else holds. Any thread.
juce::ValueTree writeSongTree (const SongState&);

// One of the root's places written whole from a state, as writeSongTree() writes it (its TRACKS with every track, its TEMPOMAP
// with the map...): an invalid tree for a place the state's root doesn't hold. What asks a part of the song whole that is one
// of them asks through WholeState.h's copyWhole() (the changes' worker, model/versions/ChangesWorker.cpp, makeSkeleton(); a
// version's MIDI links and sections, model/versions/ApplySong.cpp). Any thread.
juce::ValueTree writeSongPart (const SongState&, RootPlace);

// A state read from a song's tree as the loader made it consistent (model/project/internal/Project.h: makeConsistent()), each
// area by its reader as the song holds it (the areas' readHeld<Area>In(), a line each, in the order of the areas, the song's
// own first: each element read once, and no tree changed), its root's places in their order, and its curves' points given keys
// from 1 (CurveValues.h: PointValue; never saved nor compared). What the song takes its values from as it opens
// (Song::holdValues(), whose keys count on from those it gave: the second form). Any thread.
SongState readSongTree (const juce::ValueTree& consistent);
SongState readSongTree (const juce::ValueTree& consistent, std::uint64_t& nextPointKey);
} // namespace phazite
