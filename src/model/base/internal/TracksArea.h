// SPDX-License-Identifier: AGPL-3.0-or-later
// The tracks' area of the song's values (roadmap step S5.20; SongValues.h, whose recipe this follows), as far as it is built:
// the song's tracks, folders, returns and master are still the song's tree's (Schema.h: TRACKS, TRACK, MASTER, and a
// track's view, LANES and LANE), and their values and the conversion are TracksValue.h's (SongTracks: each track's fields
// written in the undo history alone), TrackSwitchesValue.h's (TrackSwitches: mute, solo, frozen, the automation mode, keyed)
// and TracksOutside.h's (TracksOutside: the view, outside the history). Here: what the values' listeners are told of them
// (TrackChange, TrackSwitchChange: SongValues::Listener::tracksChanged(), trackSwitchesChanged(), tracksViewChanged()); the
// tracks' part of the keeper, which tells them what the tree tells of the tracks until the tracks' switch; and what of the
// tree's notifications is the tracks' (isHeardAsTracks()), which every listener of the song's root leaves to them.
//
// What is the tracks' and what isn't, written once (isHeardAsTracks()): the song's track list (TRACKS), a track (TRACK, of the
// song's track list or of a folder in it) and the song's MASTER coming, going or moving; their own properties that the three
// values cover (the descriptions of TracksValue.h's, TrackSwitchesValue.h's and TracksOutside.h's names, each where it is
// described: a MASTER's ID, fader, VCA, volume, pan, automation mode, height, collapsed); and their LANES and LANE elements.
// Not what else is in a TRACK or the MASTER (its devices, containers and chains: the devices' area's, DevicesArea.h), nor a
// property Phazite doesn't know there, nor a property described on a TRACK alone that a MASTER holds (the loader takes those
// away), which the tree tells as before. Message thread.
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/TrackSwitchesValue.h"
#include "model/base/internal/TracksOutside.h"
#include "model/base/internal/TracksValue.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace phazite
{
// A track, a folder, a return or the master that changed (TracksValue.h): its ID; whether it is the master (which is in no
// list, its ID one a version may give anew); whether it was there and whether it is (both, for one changed in place; the
// master always); the folder it was in and is in (TrackID() for the top list, and for the master) and its place in that list
// before and after, for one that came, went or came to another folder (made, place) alone, as a search of the list finds it
// (-1 where it wasn't, or isn't, for any other change, and for the master); the parts of it that changed; and its value
// after, for one that is there, valid during the call alone: what plays may set its channel's fader from it at once
// (SongPlayer.h), as nothing else of the song is read meanwhile. Set its members by name.
struct TrackChange
{
    // The parts, as bits of parts: each its own, as what listens takes each its own way (SongPlayer.h, PlaybackSession.h).
    enum Part : std::uint16_t
    {
        made = 1 << 0,      // added or taken out: alone, or with the folder or the track list it is in; the master's element
                            // itself put in the song or taken out (a merge's choice of it whole)
        place = 1 << 1,     // come to another folder: only where the song's tracks are found again whole (the tree tells a
                            // move as a track taken out and put in again: two changes, each made)
        kind = 1 << 2,      // a track, a folder or a return
        bus = 1 << 3,       // a folder's bus switch
        fader = 1 << 4,     // its fader or its VCA (a bus made plain, or a plain folder a bus)
        name = 1 << 5,      // its name
        colour = 1 << 6,    // its colour
        mixer = 1 << 7,     // its volume or its pan
        key = 1 << 8,       // its key
        recording = 1 << 9, // its input or its monitoring
        pad = 1 << 10,      // the drum sampler a pad's own track plays a pad of, or the pad
        newID = 1 << 11     // the master's ID, given anew in place (a version's)
    };

    TrackID id {};
    bool onMaster = false;
    bool wasThere = false, isThere = false;
    TrackID parentBefore {}, parentAfter {};
    int indexBefore = -1, indexAfter = -1;
    std::uint16_t parts = 0;
    const TrackValue* now = nullptr;

    bool has (std::uint16_t part) const noexcept { return (parts & part) != 0; }
};

// An entry of the tracks' switches that changed (TrackSwitchesValue.h): which switch; the track it is of (the master's ID
// for the master's automation mode, and onMaster); and whether the entry came or went (a switch on, or off; an automation
// mode from read, or back to it), neither for a mode changed to another. Set its members by name.
struct TrackSwitchChange
{
    enum class Field : std::uint8_t
    {
        mute,
        solo,
        frozen,
        automationMode
    };

    Field field = Field::mute;
    TrackID track {};
    bool onMaster = false;
    bool appeared = false, went = false;
};

// The tracks' part of the keeper (SongValues.h), while the song's tree holds the tracks: for each of the tree's notifications
// that is the tracks' (isHeardAsTracks()), whatever the history does (an undo's and a redo's, a write's outside the history),
// the values' listeners told at once, in one call each, what it changed of each value: the tracks' (Listener::
// tracksChanged()), the switches' (trackSwitchesChanged()) and the view's (tracksViewChanged()), so that a volume and a mute
// set by hand play in the same call, as the tree's callbacks played them. Exactly what differs between the values as the
// listeners were last told them (told: at the start, every track and the master read once, as the song holds them:
// readHeldTrack(), readHeldSwitchesOf(), readHeldViewOf(), which no one is told) and as the song holds them now. The
// switches and the view keep the entries of a track taken out, as their areas will (an Undo brings the track back with
// them): a track taken out tells none of its switches, and one that comes tells those that differ from what was kept. How
// a notification is worked out, by its cost:
//   - A property of a track or the master: that one field read from that one property (TracksValue.h: setTrackProperty();
//     a key's root and scale together; TrackSwitchesValue.h: readHeldSwitchesOf() of that element alone; TracksOutside.h:
//     readHeldViewProperty()), set in the value told before (immer: a path copied), no track read whole and no element
//     walked (countTrackElementsWalked()).
//   - A track's LANES or a LANE added, taken out, moved or changed: that track's lanes read again (readHeldLanesOf()).
//   - A track (a folder with the tracks in it) added: it and the tracks in it read whole (readHeldTrack()), each element
//     walked counted, put at its place in its list (the track list's place found by JUCE's index of the element; a folder's
//     by its other tracks walked); taken out: it and the tracks in it taken out of the value, nothing read.
//   - Tracks in another order: the track list's by the two places JUCE names; a folder's by its tracks walked. A track
//     moved past none but a folder's other elements (its devices, its lanes) leaves its tracks in their order: no list is
//     told reordered, so nothing makes a plan or marks the tracks' order for it, as the tree's callbacks did (the change
//     named, 2026-11-23).
//   - The master's element put in the song or taken out: the master read again (made).
//   - Where the value can't be counted on to follow one element (a track given another ID or another kind in place, the
//     track list itself put in or taken out, an element of an ID another the song holds already has, or one the value
//     wasn't read from): the song's tracks read again whole, as at the start, and what differs told.
// The calls are the tree's (one for each of its notifications that changes a value, none for one that changes nothing of
// them), so that what a step told in all, not how many calls, is what holds from the tracks' switch on. Nothing of the song's
// values is read.
struct SongValues::TracksKeeper
{
    TracksKeeper (SongValues&, Song&);

    // The keeper's callbacks, each for every element, whoever's it is, in an undo or a redo too: nothing, at once, but for
    // what is the tracks'.
    void propertyChanged (const juce::ValueTree& tree, const juce::Identifier& property);
    void childAdded (const juce::ValueTree& parent, const juce::ValueTree& child);
    void childRemoved (const juce::ValueTree& parent, const juce::ValueTree& child);
    void childOrderChanged (const juce::ValueTree& parent, int oldIndex, int newIndex);

    SongValues& values;
    Song& song;
    SongTracks told {};        // the song's tracks as the listeners were last told them
    TrackSwitches switches {}; // and their switches, the entries of tracks taken out kept
    TracksOutside view {};     // and their view, the same

    // Each of told's tracks' element, by its ID.
    std::unordered_map<std::uint64_t, juce::ValueTree> elements {};
    bool masterMade = false; // the master's element put in or taken out since it was last told

    const TrackValue* findKept (const juce::ValueTree& track) const; // told's value of the element, or nullptr
    bool isKept (const juce::ValueTree& track) const;
    void takeValueProperty (const juce::ValueTree& element, const juce::Identifier& property);
    void takeSwitches (const juce::ValueTree& element);
    void takeViewProperty (const juce::ValueTree& element, const juce::Identifier& property);
    void takeLanes (const juce::ValueTree& channel);
    void add (const juce::ValueTree& list, const juce::ValueTree& track);
    void remove (const juce::ValueTree& track);
    void reorder (const juce::ValueTree& list, int oldIndex, int newIndex);
    void takeMaster();
    void settle();                                                            // the song's tracks read again whole
    std::optional<TrackID> findListOwner (const juce::ValueTree& list) const; // TrackID() for the track list's

    void tell (const SongTracks& before, const TrackSwitches& switchesBefore, const TracksOutside& viewBefore);

    JUCE_DECLARE_NON_COPYABLE (TracksKeeper)
};

// Whether what the song's tree tells is the tracks', which a listener of the song's root hears from the values' listener
// alone (SongValues::Listener::tracksChanged(), trackSwitchesChanged(), tracksViewChanged()) and leaves alone in its tree
// callbacks, while the tree is still their storage (the top of this header). Of an element (children in another order): the
// song's track list, a track of the song's, its master, a track's or the master's LANES. Of a property changed: one of the
// three values' (TracksValue.h, TrackSwitchesValue.h, TracksOutside.h) described on a track of the song's, or on its master,
// or a LANE's of one. Of a child added or taken out: a track to the song's track list or a folder of it, the track list or
// the master to the song, a LANES to a track or the master, or what is in one. In TracksArea.cpp.
bool isHeardAsTracks (const juce::ValueTree& element);
bool isHeardAsTracks (const juce::ValueTree& element, const juce::Identifier& property);
bool isHeardAsTracks (const juce::ValueTree& parent, const juce::ValueTree& child);

// The song's tracks, their switches and their view as its tree holds them, read as the song holds them (readHeldTrack(),
// readHeldSwitchesOf(), readHeldViewOf()): each TRACK of the track list and, in a folder, the TRACKs in it (a folder's before
// them), each list in its element's order; of two of one ID, the first, without what is in it; one whose ID the song can't
// count on left out with what is in it; and the master (as a song without a MASTER has it, where it has none). What the
// tracks' keeper starts from, and what the tests' check compares what it told with (tests/SongCheck.cpp).
SongTracks readHeldTracks (const juce::ValueTree& song);
TrackSwitches readHeldTrackSwitches (const juce::ValueTree& song);
TracksOutside readHeldTracksOutside (const juce::ValueTree& song);

// How many TRACK elements the tracks' keeper looked at in its walks (a track added with the tracks in it, a folder's tracks
// in another order, the song's tracks read again), by every song since the program started: for the speed gates
// (tests/PerformanceGateTests.cpp), which count what an edit makes it walk. Any thread.
std::uint64_t countTrackElementsWalked() noexcept;

// How many entries of the tracks' lists (a folder's, the top list's) the tracks' keeper compared as it worked out what to tell
// (a track's place in its list, a list's order before and after), by every song since the program started: for the speed
// gates (tests/PerformanceGateTests.cpp). Any thread.
std::uint64_t countTrackListEntriesCompared() noexcept;
} // namespace phazite
