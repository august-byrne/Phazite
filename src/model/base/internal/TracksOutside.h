// SPDX-License-Identifier: AGPL-3.0-or-later
// What the song holds of its tracks outside its undo history alone, as a value (held
// in an OutsideValue, OutsideValue.h): how each track's row shows, view state, which counts as no change and no step holds
// (decision 4): its height, whether it is collapsed or pinned, its drum mode and drum rows (Schema.h: trackHeight,
// trackCollapsed, trackPinned, drumMode, drumOrder, drumHidden) and its automation lanes (LANES, LANE), the master's row's
// too. With the conversion between those properties and elements of a TRACK and the MASTER and the value.
//
// The song holds it outside its history (SongValues.h: tracksOutside; TracksArea.h): the functions by ID that read and write it
// are in model/arrangement/internal/Tracks.h (getTrackView(), getViewOf(), setTrackView(), getSavedTrackHeight(), isCollapsed(),
// isPinned() and their setters), model/notes/internal/Notes.h (the drum rows) and model/automation/internal/Automation.h
// (getLanes() and the lanes' writers). The conversion below is what a file's, a whole copy's or a version's element is read from
// and written into. immer is the model's alone (tools/check-architecture.sh: immer-outside-model).
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/Immer.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <functional>
#include <optional>

namespace phazite
{
// An automation lane the playlist shows under a track (Schema.h: LANE), in its order: the device of the parameter (a
// device's, or a fader's or a VCA's: laneDevice), the parameter as its curve names it (laneParameter), its height where one
// was set (laneHeight: none for the lane's default, AutomationLane::defaultHeight, as a lane kept but never resized has
// none, and the lanes' writer writes none for a height set to it; a file's 48 is read as it is) and whether it is a thin row
// (laneCollapsed).
struct LaneValue
{
    DeviceID device {};
    juce::String parameter {};
    std::optional<int> height {};
    bool collapsed = false;

    bool operator== (const LaneValue&) const = default;
};

// How a track's row shows (view, uncounted): no undo step and no change. Each property none until it is set, as the readers
// tell none from a value (getSavedTrackHeight()); collapsed false once it was set off (setCollapsed() writes false, as
// Phazite always wrote it: the golden command script holds collapsed="0"), pinned none while it is off (setPinned() writes
// none for off; a file's "0" is read as it is, false, and written back so); the drum mode as it was
// set, false too (none is as the track's instrument says); the drum rows' texts as a file holds them, none for none (the
// writers write none for an empty list); its lanes in their order, none while it has none (no empty LANES is written:
// writeViewInto()).
struct TrackView
{
    std::optional<int> height {};              // trackHeight
    std::optional<bool> collapsed {};          // trackCollapsed
    std::optional<bool> pinned {};             // trackPinned
    std::optional<bool> drumMode {};           // drumMode: none = on while its instrument is the drum sampler
    std::optional<juce::String> drumOrder {};  // drumOrder
    std::optional<juce::String> drumHidden {}; // drumHidden
    immer::flex_vector<LaneValue> lanes {};    // LANES' LANEs

    bool operator== (const TrackView&) const = default;
};

// Each track's view by its ID, and the master's row's, whatever its ID (which a version applied sets anew). Entries of
// tracks the song doesn't hold (one a step took away, which its Undo may bring back) are kept, and never written.
struct TracksOutside
{
    immer::map<std::uint64_t, TrackView> tracks {};
    TrackView master {};

    // The same map without a look inside, the master's alike (OutsideValue.h); and the same views, compared.
    bool isSameAs (const TracksOutside&) const noexcept;
    bool operator== (const TracksOutside&) const;
};

// The value with one track's view set, by its ID (an empty view, which shows as none set, takes its entry away), or the
// master's: the very value given where it holds that view so already (a look in one map), so that a set of it is no change.
// What every writer of a row's view sets it by (SongValues.h: setTracksOutside(); model/arrangement/internal/Tracks.h:
// setTrackView()).
TracksOutside withTrackView (const TracksOutside&, std::uint64_t id, TrackView);
TracksOutside withMasterView (const TracksOutside&, TrackView);

//==============================================================================
// The conversion: the one place that reads and writes a TRACK's and the MASTER's view, and LANES and LANE, by name.

// A TRACK's or the MASTER's view as Phazite's code always read it (model/arrangement/Tracks.cpp, model/notes/Notes.cpp,
// model/automation/Automation.cpp): its height as an int truncates it, where it has one; collapsed, pinned and drum mode as a
// cast's bool where it has each; the drum rows' texts where it has them; and each LANE of its LANES (readLaneValue()), in their
// order. Every one the element holds, wherever it is described (a MASTER's pinned too). An empty view for any other element.
// Called by the song's check (tests/SongCheck.cpp) and the tests alone, against readHeldViewOf().
TrackView readViewOf (const juce::ValueTree& trackOrMaster);

// The same as the song holds it: only what is described on the element's type (a MASTER's height, collapsed and lanes), its
// height within its range, made whole, none for what is no finite number; each lane as readHeldLane() reads it, a lane that
// reads as none left out.
TrackView readHeldViewOf (const juce::ValueTree& trackOrMaster);

// A LANE as Phazite's code always read it (model/automation/Automation.cpp: getLanes()): its device as a cast reads its text, its
// parameter as text, its height as an int truncates it where it has one, collapsed as a cast's bool. And as the song holds it:
// its device one the song can count on from and a parameter that isn't empty (Schema.h: laneParameter, always), else none (no
// lane); its height within its range, made whole: readHeldLane(). The first called by readViewOf() alone (the song's check and
// the tests).
LaneValue readLaneValue (const juce::ValueTree& lane);
std::optional<LaneValue> readHeldLane (const juce::ValueTree& lane);

// A track's view written into its element, which holds none of it, as Phazite's writers write it
// (model/arrangement/Tracks.cpp, model/notes/Notes.cpp, model/automation/Automation.cpp): each property where it has one,
// in its type; and where it has lanes, a LANES after the element's other children, a LANE for each in their order: its device
// as its ID's text, its parameter, its height where it has one, collapsed only while on. No LANES for no lanes.
void writeViewInto (juce::ValueTree& trackOrMaster, const TrackView&);

// The element's view made as writeViewInto() writes what readHeldViewOf() reads of it, in place: each view property it
// holds set where it holds another text, each the writer leaves out taken away; each LANE of its LANES that reads as a lane
// made so (its device as its ID's text, collapsed off as none). Its LANES stays where it is, empty or not, and so does each
// LANE, in its order (the loader leaves what names nothing out first: Project.h, makeConsistent()). The rest as it is.
// Nothing for an element whose view is so already, and no allocation then. Whether it changed. The loader's, with
// keepTrackAsHeld() (TracksValue.h) and keepSwitchesAsHeld() (TrackSwitchesValue.h).
bool keepViewAsHeld (juce::ValueTree trackOrMaster);

// Whether a property of a TRACK or the MASTER is one of the view's described on its type.
bool isViewProperty (const juce::ValueTree& trackOrMaster, const juce::Identifier& property); // described on its type

// Each device a lane names (laneDevice), of every track's and the master's: what goes with the device (IDsHeld.h).
void forEachIDNamed (const TracksOutside&, const std::function<void (std::uint64_t)>& each);

// One field of one element as readViewOf() reads it, without reading the rest: what the conversion reads each with. None of an
// element that is no TRACK or MASTER.
std::optional<int> readSavedHeight (const juce::ValueTree& trackOrMaster);
std::optional<bool> readCollapsed (const juce::ValueTree& trackOrMaster);
std::optional<bool> readPinned (const juce::ValueTree& trackOrMaster);
std::optional<bool> readDrumMode (const juce::ValueTree& trackOrMaster);
} // namespace phazite
