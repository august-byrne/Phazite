// SPDX-License-Identifier: AGPL-3.0-or-later
// The tracks' area of the song's values (roadmap step S5.20; SongValues.h, whose recipe this follows): the song's tracks,
// folders, returns and master are values beside the song's tree, in three areas: their fields written in the undo history
// alone (SongValues.h: tracks, TracksValue.h: SongTracks, plain), their switches (trackSwitches, TrackSwitchesValue.h: mute,
// solo, frozen, the automation mode, keyed) and their view (tracksOutside, TracksOutside.h: the view, LANES and LANE, outside
// the history). The song's tree keeps its TRACKS and MASTER as places, empty (a TRACKS holds no TRACK, the MASTER no property),
// which whole state writes the tracks into (WholeState.cpp: writeTracksIn()). Here: what the values' listeners are told of them
// (TrackChange, TrackSwitchChange: SongValues::Listener::tracksChanged(), trackSwitchesChanged(), tracksViewChanged()) and the
// three tellers; the tracks' setters (members of SongValues) and their order (SongValues::getTrackOrder()); the tracks' part of
// the keeper (its safety net, and the adoption) and the switches' and the view's; their part of whole state and of the
// comparison; and what of the tree's notifications is the tracks' (isHeardAsTracks()), which every listener of the song's root
// leaves to them.
//
// What is the tracks' and what isn't, written once (isHeardAsTracks()): the song's TRACKS and MASTER themselves, a TRACK added
// to the TRACKS, a property of the MASTER, and a LANES or a LANE added to the MASTER (what the keeper takes in, its safety
// net). Not what else is in the MASTER (a lone device: the devices' area's safety net, DevicesArea.h), which the tree tells as
// before. Message thread, but for whole state's parts and the comparison, which read only the values they are given.
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/TrackSwitchesValue.h"
#include "model/base/internal/TracksOutside.h"
#include "model/base/internal/TracksValue.h"
#include "model/base/internal/WholeState.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <optional>
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
        place = 1 << 1,     // come to another folder: a move is one change of the track, never made (the tracks in a folder
                            // moved are told nothing: their folder is the same)
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

// What the values' listeners are told of the tracks (Listener::tracksChanged()): once for each change of the area's value (a
// step's, an undo's or a redo's whole step, a write outside the history), what differs between the two: each track added,
// taken out, come to another folder or changed (immer's diff of the map, which skips what the two share: O(changed)), with its
// folder and, for one that came, went or came to another folder alone, its place in its list before and after, as a search of
// the list finds it (countTrackListEntriesCompared()); the master where it differs, or where it was put in anew whole
// (SongTracks::masterPutIn: made); each list whose tracks came in another order (immer's diff of the lists); and the area's
// whole value after (cheap: it shares what it holds). It listens to the area from its making to its end.
struct SongValues::TracksTeller final : ValueArea<SongTracks>::Listener
{
    TracksTeller (ValueArea<SongTracks>&, juce::ListenerList<SongValues::Listener>&);
    ~TracksTeller() override;

    ValueArea<SongTracks>& area;
    juce::ListenerList<SongValues::Listener>& listeners;

    void valueAreaChanged (const SongTracks& before, const SongTracks& after) override;

    JUCE_DECLARE_NON_COPYABLE (TracksTeller)
};

// The tracks' part of the keeper (SongValues.h): the song's tree holds the tracks no more (its TRACKS holds no TRACK, its MASTER
// no property: places, as the TEMPOMAP is), and this keeps them so, the safety net of the recipe's step 13 for a placeholder:
//   (a) a TRACK added to the song's TRACKS, a TRACKS or a MASTER added to the song, or a property written to its MASTER: in a
//       step, refused, the element or the property left where it was put (a JUCE assertion: the tracks' setters change the
//       tracks, model/arrangement/internal/Tracks.h; the step that adds it can't take it out again, as its Undo would find
//       it gone: JUCE workaround W62 (docs/juce-workarounds.md)), and no other area's part asked of it; outside the history,
//       read in as the song holds it, each TRACK (a folder's too) and the MASTER read once (TracksValue.h: readHeldTrack()),
//       a track of an ID the song holds or can't hold left out with the tracks in it; their switches and view read in
//       (TrackSwitchesValue.h: readHeldSwitchesOf(); TracksOutside.h: readHeldViewOf()); what each holds read in through
//       each area's reader of a whole element and set through its setter (its CLIPs with their notes, ClipsArea.h:
//       takeTrackClipsIn(); its DEVICEs and CONTAINERs with what the store holds of them, DevicesArea.h:
//       takeChannelDevicesIn(), DeviceStoreArea.h: takeChannelStoreIn(); its MODULATORs and MACROs, a container's too,
//       ModulationArea.h: takeChannelModulatorsIn(); its SENDs, SendsArea.h: takeTrackSendsIn()); the tracks set after the
//       last of the top list (the TRACKS holds no other to place it by); the element taken out (a property taken away), and
//       each counted (countTracksAdopted()). The other areas' parts are not asked of what this takes (SongValues.cpp: the
//       Keeper), so that none of them relies on its own case of a TRACK.
//   (b) nothing as a track leaves: no TRACK is in the tree to leave it (the tracks' setters take a track out of the value).
// And the adoption from the loaded tree, which the song's holdValues() asks last, after every other area's: the song's tracks
// read once each, as the song holds them (readHeldTracksIn(), before any area's elements are taken out), into the area outside
// the history; then every TRACK out of the
// TRACKS and every property and child out of the MASTER (what the other areas take of them taken first; what Phazite doesn't
// know of a TRACK or the MASTER dropped as the song opens: docs/project-format.md).
struct SongValues::TracksKeeper
{
    TracksKeeper (SongValues&, Song&);

    // A child added to an element of the song's tree and a property changed (the keeper's callbacks, but while the history
    // undoes or redoes, for every element, whoever's it is): whether it was the tracks' (a), which the keeper then asks no
    // other area's part of.
    bool childAdded (juce::ValueTree& parent, juce::ValueTree& child);
    bool propertyChanged (juce::ValueTree& tree, const juce::Identifier& property);

    static void adopt (SongValues& into, Song& loaded);

    SongValues& values;
    Song& song;
    juce::ValueTree takingOut {}; // what the keeper takes out (or takes properties out of) as it is told: none of its own to read

    JUCE_DECLARE_NON_COPYABLE (TracksKeeper)
};

// What the values' listeners are told of the switches (Listener::trackSwitchesChanged()): once for each change of the area's
// value (a step's, an undo's or a redo's whole step, a write outside the history), each entry that differs between the two
// (MapChanges.h, through immer's diff, which skips what the two share), the master's mode named by the master's ID as the
// tracks' area holds it, and the area's whole value after (cheap: it shares what it holds). It listens to the area from its
// making to its end.
struct SongValues::TrackSwitchesTeller final : ValueArea<TrackSwitches>::Listener
{
    TrackSwitchesTeller (SongValues&, juce::ListenerList<SongValues::Listener>&);
    ~TrackSwitchesTeller() override;

    SongValues& values; // the master's ID, as the tracks' area holds it
    ValueArea<TrackSwitches>& area;
    juce::ListenerList<SongValues::Listener>& listeners;

    void valueAreaChanged (const TrackSwitches& before, const TrackSwitches& after) override;

    JUCE_DECLARE_NON_COPYABLE (TrackSwitchesTeller)
};

// What the values' listeners are told of the view (Listener::tracksViewChanged()): once for each write that changes it, each
// track whose row differs between the two (immer's diff of the map) that the song holds as the tracks' area holds them, and
// whether the master's did. Nothing of a track the song doesn't hold: one a maker gives its view before it puts the track in
// the song (its row is new, told by tracksChanged()), or one a step took away (its entry kept for an Undo).
struct SongValues::TracksViewTeller final : OutsideValue<TracksOutside>::Listener
{
    TracksViewTeller (SongValues&, juce::ListenerList<SongValues::Listener>&);
    ~TracksViewTeller() override;

    SongValues& values; // which tracks the song holds, as the tracks' area holds them
    OutsideValue<TracksOutside>& area;
    juce::ListenerList<SongValues::Listener>& listeners;

    void outsideValueChanged (const TracksOutside& before, const TracksOutside& after) override;

    JUCE_DECLARE_NON_COPYABLE (TracksViewTeller)
};

// The switches' and the view's part of the keeper (SongValues.h), in the mode of the moment, nothing while the history undoes
// or redoes (a TRACK, the track list or the MASTER that comes in with a switch, a view property or a LANES is the tracks' part
// of the keeper's, which reads them in with it: TracksKeeper):
//   (b) a lone LANES added to the MASTER, or a lone LANE to its LANES: refused in a step, whose Undo, taking the tree's change
//       back first, would find it gone (a JUCE assertion: the lanes' functions change the lanes,
//       model/automation/internal/Automation.h; JUCE workaround W62 (docs/juce-workarounds.md)); read in outside the history
//       (the lanes it holds made the master's, after those it has), counted, and taken out;
//   (c) nothing as a track leaves: the switches and the view keep its entries (an Undo brings the track back with them).
// And the adoption from the loaded tree, which the song's holdValues() asks.
struct SongValues::TrackLeavesKeeper
{
    TrackLeavesKeeper (SongValues&, Song&);

    // A child added to an element of the song's tree (the keeper's callback, but while the history undoes or redoes, for every
    // element, whoever's it is): nothing, at once, but for what is the switches' or the view's.
    void childAdded (juce::ValueTree& parent, juce::ValueTree& child);

    // What every TRACK of the track list (a folder's tracks too) and the MASTER of the tree the loader made consistent holds of
    // the switches and the view out of the tree, once its reader read them into the switches' area and the view's value as the
    // song holds them (readHeldTrackLeavesIn(); each of which the loader left as it holds it, asserted here:
    // TrackSwitchesValue.h, keepSwitchesAsHeld(); TracksOutside.h, keepViewAsHeld(); SongValues.cpp: adopt()).
    static void adopt (SongValues& into, Song& loaded);

    SongValues& values;
    Song& song;

    JUCE_DECLARE_NON_COPYABLE (TrackLeavesKeeper)
};

// The tracks' part of whole state (WholeState.cpp: writeIn(), which writes each TRACK whole: writeTracksIn()): whether a child
// of an element in a copy of the song is the tracks' place (the song's TRACKS or MASTER); and a copy's TRACK given its switches
// (TrackSwitchesValue.h: writeSwitchesInto(), its ID's entries) and its view (TracksOutside.h: writeViewInto(): its view's
// properties, and a LANES after the element's other children where it has a lane), the MASTER its mode and its view, from the
// values kept. Whole state writes them after a TRACK's clips, its devices and a folder's tracks, before its modulators and its
// sends, as every file Phazite wrote holds them. Nothing for another element.
bool leadsToTracks (const juce::ValueTree& parent, const juce::ValueTree& child);
void writeTrackLeavesIn (juce::ValueTree& element, const SongState&);

// The tracks' readers of a song's tree (SongState.h: readSongTree(), a line each): each track's and the master's switches and
// view into the state; and the tracks themselves, each read once as the song holds it. No tree changed. Any thread.
void readHeldTrackLeavesIn (const juce::ValueTree& song, SongState&);
void readHeldTracksIn (const juce::ValueTree& song, SongState&);

// Whether what the song's tree tells is the tracks', which a listener of the song's root hears from the values' listener
// alone (SongValues::Listener::tracksChanged(), trackSwitchesChanged(), tracksViewChanged()) and leaves alone in its tree
// callbacks: the places and what the keeper takes in there (the top of this header). Of an element (children in another
// order): the song's TRACKS, its MASTER, the MASTER's LANES. Of a property changed: any of the song's MASTER (it holds none but
// what its keeper takes out), or a LANE's of the MASTER's LANES. Of a child added or taken out: a TRACK to the song's TRACKS,
// the TRACKS or the MASTER to the song, a LANES to the MASTER, a LANE to its LANES. In TracksArea.cpp.
bool isHeardAsTracks (const juce::ValueTree& element);
bool isHeardAsTracks (const juce::ValueTree& element, const juce::Identifier& property);
bool isHeardAsTracks (const juce::ValueTree& parent, const juce::ValueTree& child);

// A song's tracks as its tree holds them (a file's, a whole copy's: the live song's tree holds none), read as the song holds
// them (readHeldTrack()): each TRACK of the track list and, in a folder, the TRACKs in it (a folder's before them), each list
// in its element's order; of two of one ID, the first, without what is in it; one whose ID the song can't count on left out
// with what is in it; and the master (as a song without a MASTER has it, where it has none). What the adoption reads once,
// and the tests.
SongTracks readHeldTracks (const juce::ValueTree& song);

// How many TRACK elements the tracks' reading of a tree looked at in its walks (the adoption, readHeldTracks()), by every song
// since the program started: for the speed gates (tests/PerformanceGateTests.cpp), which count what an edit makes it walk
// (none: the live song's tree holds no track). Any thread.
std::uint64_t countTrackElementsWalked() noexcept;

// How many entries of the tracks' lists (a folder's, the top list's) the tracks' teller compared as it worked out what to tell
// (a track's place in its list, a list's order before and after), by every song since the program started: for the speed
// gates (tests/PerformanceGateTests.cpp). Any thread.
std::uint64_t countTrackListEntriesCompared() noexcept;
} // namespace phazite
