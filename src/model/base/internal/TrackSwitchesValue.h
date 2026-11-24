// SPDX-License-Identifier: AGPL-3.0-or-later
// What the song holds of its tracks that is written both in its undo history and outside it, as a keyed value (roadmap
// step S5.20, the tracks' area; ValueArea.h, KeyedAreaValue): each track's mute, solo and frozen switch (Schema.h:
// trackMute, trackSolo, trackFrozen) and each track's and the master's automation mode (trackAutomationMode), each an entry
// by its own key, so that an Undo puts back only the entries its step changed, as the tree undoes one property at a time: a
// solo set by hand after Bounce in Place survives the bounce's Undo, which puts back the mute and the freeze of the track it
// rendered (design J.1). The toggles, the freeze keeper and the automation writer write them outside the history; Bounce in
// Place, a version applied and a merge's choice in a step. The tracks' plain value covers the rest of a track
// (TracksValue.h); its view is outside the history alone (TracksOutside.h). With the conversion between those properties of
// a TRACK and the MASTER and the switches.
//
// The song holds them in a keyed area (SongValues.h: trackSwitches; TracksArea.h), and its tree's TRACK and MASTER elements
// hold none of them: the functions by ID that read and write them are in model/arrangement/internal/Tracks.h
// (isTrackMuted(), isTrackSoloed(), isTrackFrozen(), setTrackMuted(), setTrackSoloed(), setTrackFrozen(),
// getTrackSwitches(), setTrackSwitches()) and model/automation/internal/Automation.h (getAutomationMode(),
// setAutomationMode()). The conversion below is what a file's, a whole copy's or a version's element is read from and written
// into (the loader, the adoption, the keeper's safety net, whole state, the versions' pass). Its own header, beside the other
// areas' (one header for each area's values); immer is the model's alone (tools/check-architecture.sh: immer-outside-model).
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/Immer.h"
#include "model/base/internal/MapChanges.h"
#include "model/base/internal/ValueArea.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace phazite
{
// How a track writes its automation as the song plays, as the song saves it (Schema.h: trackAutomationMode); none is read.
// The automation's own AutomationMode (model/automation/AutomationValues.h) names read too.
enum class SavedAutomationMode : std::uint8_t
{
    touch,
    latch,
    write
};

// Each track's switches by its ID (the master's automation mode under 0, whatever its ID, which a version applied sets
// anew): its mute, its solo and its frozen switch, each held only while on (off is none, as the tree removes them), and
// its automation mode, none for read. Entries of tracks the song doesn't hold (a track a step took away, which its Undo may
// bring back) are kept, and never written into a song that doesn't hold the track (writeSwitchesInto() writes one track's).
struct TrackSwitches
{
    static constexpr bool undoesByEntry = true;

    immer::map<std::uint64_t, bool> mutes {};                // trackMute
    immer::map<std::uint64_t, bool> solos {};                // trackSolo
    immer::map<std::uint64_t, bool> frozen {};               // trackFrozen
    immer::map<std::uint64_t, SavedAutomationMode> modes {}; // trackAutomationMode; the master's under 0

    // The same maps without a look inside (ValueArea.h); and the same entries, compared.
    bool isSameAs (const TrackSwitches&) const noexcept;
    bool operator== (const TrackSwitches&) const;
};

// The entries that differ between two values of the switches, a list for each map (MapChanges.h); and a value with exactly
// those moved back or forward: the keyed value's functions (ValueArea.h).
struct TrackSwitchesChanges
{
    std::vector<MapChange<std::uint64_t, bool>> mutes {}, solos {}, frozen {};
    std::vector<MapChange<std::uint64_t, SavedAutomationMode>> modes {};
};

TrackSwitchesChanges findChanges (const TrackSwitches& before, const TrackSwitches& after);
TrackSwitches withChanges (TrackSwitches now, const TrackSwitchesChanges&, ChangeDirection);

// The switches with one entry of one key set (a switch on, or off: none), or the mode (none for read): the very value given
// where it holds that entry so already, so that a set of it is no change (a look in one map, no value compared): what a
// toggle, the freeze keeper and the automation writer set one switch by (SongValues.h: setSwitchesOutsideHistory()).
TrackSwitches withMute (const TrackSwitches&, std::uint64_t key, bool on);
TrackSwitches withSolo (const TrackSwitches&, std::uint64_t key, bool on);
TrackSwitches withFrozen (const TrackSwitches&, std::uint64_t key, bool on);
TrackSwitches withAutomationMode (const TrackSwitches&, std::uint64_t key, std::optional<SavedAutomationMode>);

// Whether two values hold the same entries of one key (a track's ID, the master's 0), each of the four maps: what Project's
// comparison with the saved copy compares a track's switches by (WholeState.h).
bool holdSameSwitchesOf (const TrackSwitches&, const TrackSwitches&, std::uint64_t key);

// Whether the switches hold any entry of one key (a mute, a solo, a frozen switch or a mode kept for it): none for a track
// new to the song, and for one that held every switch off. What a track that comes in through the versions or a merge is
// given its switches by (model/versions/ApplySong.cpp: makeTrackLeavesLike(); model/arrangement/Tracks.cpp:
// chooseTrackClash()): outside the history where the switches hold none of its ID and the song didn't hold it before, as a
// maker gives a new item's (model/plugins/DeviceStore.cpp: setMadeStore()), else in the step, whose Undo puts back what a
// track an earlier step took away kept.
bool holdsSwitchesOf (const TrackSwitches&, std::uint64_t key);

//==============================================================================
// The conversion: the one place that reads and writes a TRACK's and the MASTER's switches by name.

// What a TRACK or the MASTER holds of the switches, put into the switches under its key (a track's ID as a cast reads it, 0
// for the master), as Phazite's code always read it (model/arrangement/Tracks.cpp, model/mixer/Freeze.cpp,
// model/automation/Automation.cpp): each switch where it reads as on (a cast's bool: "1", "true"), its entry taken away where
// it doesn't; an automation mode by its word, none (read) for none and any other word. A TRACK's four, the MASTER's mode.
// Nothing for any other element, nor for a track without an ID.
void readSwitchesOf (const juce::ValueTree& trackOrMaster, TrackSwitches&);

// The same as the song holds it: what enters the switches (from the tracks' switch on): a track's ID one the song can count
// on from (more than 0, below schema::maxID), else nothing read; each switch and the mode as readSwitchesOf() reads them
// (the loader leaves a mode one of its words, and every switch a bool: Project.h, makeConsistent()).
void readHeldSwitchesOf (const juce::ValueTree& trackOrMaster, TrackSwitches&);

// The switches' entries of one track (by its ID) or of the master (by 0) written into its element, which holds none of
// them, as Phazite's writers write them (model/arrangement/Tracks.cpp: setTrackMuted(), setTrackSoloed();
// model/mixer/Freeze.cpp: freezeFromFile(); model/automation/Automation.cpp: setAutomationMode()): a TRACK's mute, solo and
// frozen switch each only while on, as true, its mode as its word but for read; the MASTER's mode.
void writeSwitchesInto (juce::ValueTree& trackOrMaster, const TrackSwitches&);

// The element's switches made as writeSwitchesInto() writes what readHeldSwitchesOf() reads of it: each set where it holds
// another text, each the writer leaves out taken away (a switch written off: mute="0"). The rest as it is. Nothing for an
// element whose switches are so already, and no allocation then. Whether it changed. The loader's, with keepTrackAsHeld()
// (TracksValue.h) and keepViewAsHeld() (TracksOutside.h).
bool keepSwitchesAsHeld (juce::ValueTree trackOrMaster);

// Whether a TRACK or the MASTER holds any of the switches' properties (a TRACK's four, the MASTER's mode), and whether a
// property is one of them on its type (a merge's clash on one: model/arrangement/Tracks.cpp, chooseTrackClash()); and those
// taken out of it, with an undo manager (a step's) or none: what the adoption and the keeper's safety net take out of an
// element that comes with them (TracksArea.h), and a merge's whole track out of its copy (chooseTrackClash()). Nothing for any
// other element.
bool holdsSwitchesIn (const juce::ValueTree& trackOrMaster);
bool isSwitchProperty (const juce::ValueTree& trackOrMaster, const juce::Identifier& property);
void takeSwitchesOutOf (juce::ValueTree trackOrMaster, juce::UndoManager*);

// Every track's and the master's switches of a song's tree (PHAZITE), read as readSwitchesOf() reads them, in the song's
// order (the order makes no difference to the value). For the tests, and what reads them whole while the tree holds them.
TrackSwitches readTrackSwitches (const juce::ValueTree& song);

// One switch of one element as readSwitchesOf() reads it, without reading the others: what the readers by ID read them
// by, up the folders at every paint of a row. A switch of an element that is no TRACK, and a mode of one that is no TRACK or
// MASTER, read as none.
bool readTrackMute (const juce::ValueTree& element);
bool readTrackSolo (const juce::ValueTree& element);
bool readTrackFrozen (const juce::ValueTree& element);
std::optional<SavedAutomationMode> readSavedAutomationMode (const juce::ValueTree& element);
} // namespace phazite
