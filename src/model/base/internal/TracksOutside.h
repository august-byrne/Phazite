// SPDX-License-Identifier: AGPL-3.0-or-later
// What the song holds of its tracks outside its undo history alone, as a value (roadmap step S5.20, the tracks' area; held
// in an OutsideValue, OutsideValue.h): how each track's row shows, view state, which counts as no change and no step holds
// (decision 4): its height, whether it is collapsed or pinned, its drum mode and drum rows (Schema.h: trackHeight,
// trackCollapsed, trackPinned, drumMode, drumOrder, drumHidden) and its automation lanes (LANES, LANE), the master's row's
// too. With the conversion between those properties and elements of a TRACK and the MASTER and the value.
//
// The song's tree is still the storage, and the song holds no such value yet (SongValues.h): the functions by ID that read
// the view on the tree are in model/arrangement/internal/Tracks.h (getSavedTrackHeight(), isCollapsed(), isPinned()),
// model/notes/internal/Notes.h (the drum rows) and model/automation/internal/Automation.h (getLanes()), each through the
// readers of one field below. immer is the model's alone (tools/check-architecture.sh: immer-outside-model).
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
// none) and whether it is a thin row (laneCollapsed).
struct LaneValue
{
    DeviceID device {};
    juce::String parameter {};
    std::optional<int> height {};
    bool collapsed = false;

    bool operator== (const LaneValue&) const = default;
};

// How a track's row shows (view, uncounted): no undo step and no change. Each property none until it is set, as the readers
// tell none from a value (getSavedTrackHeight()) and the writers write false too (setCollapsed(), setPinned()); the drum
// rows' texts as they were written (an empty order too); its lanes in their order, none while it has none (an empty LANES,
// which the tree keeps once a lane was kept, is none here: writeViewInto() writes none).
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
// tracks the song doesn't hold (one a step took away, which its Undo may bring back) are kept, as the tree keeps a removed
// element's, and never written.
struct TracksOutside
{
    immer::map<std::uint64_t, TrackView> tracks {};
    TrackView master {};

    // The same map without a look inside, the master's alike (OutsideValue.h); and the same views, compared.
    bool isSameAs (const TracksOutside&) const noexcept;
    bool operator== (const TracksOutside&) const;
};

//==============================================================================
// The conversion: the one place that reads and writes a TRACK's and the MASTER's view, and LANES and LANE, by name.

// A TRACK's or the MASTER's view as Phazite's code always read it (model/arrangement/Tracks.cpp, model/notes/Notes.cpp,
// model/automation/Automation.cpp): its height as an int truncates it, where it has one; collapsed, pinned and drum mode as a
// cast's bool where it has each; the drum rows' texts where it has them; and each LANE of its LANES (readLaneValue()), in
// their order. Every one the element holds, wherever it is described (a MASTER's pinned too). An empty view for any other
// element.
TrackView readViewOf (const juce::ValueTree& trackOrMaster);

// The same as the song holds it: only what is described on the element's type (a MASTER's height, collapsed and lanes), its
// height within its range, made whole, none for what is no finite number; each lane as readHeldLane() reads it, a lane that
// reads as none left out.
TrackView readHeldViewOf (const juce::ValueTree& trackOrMaster);

// A LANE as Phazite's code always read it (model/automation/Automation.cpp: getLanes()): its device as a cast reads its
// text, its parameter as text, its height as an int truncates it where it has one, collapsed as a cast's bool. And as the
// song holds it: its device one the song can count on from, else none (no lane); its height within its range, made whole.
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

// Every track's view and the master's of a song's tree (PHAZITE), read as readViewOf() reads them. For the tests, and what
// reads them whole while the tree holds them.
TracksOutside readTracksOutside (const juce::ValueTree& song);

// Each device a lane names (laneDevice), of every track's and the master's: what goes with the device (IDsHeld.h).
void forEachIDNamed (const TracksOutside&, const std::function<void (std::uint64_t)>& each);

// One field of one element as readViewOf() reads it, without reading the rest: what the readers by ID read the view by at
// every row of the playlist. None of an element that is no TRACK or MASTER.
std::optional<int> readSavedHeight (const juce::ValueTree& trackOrMaster);
std::optional<bool> readCollapsed (const juce::ValueTree& trackOrMaster);
std::optional<bool> readPinned (const juce::ValueTree& trackOrMaster);
std::optional<bool> readDrumMode (const juce::ValueTree& trackOrMaster);
juce::String readDrumOrder (const juce::ValueTree& trackOrMaster);  // empty for none
juce::String readDrumHidden (const juce::ValueTree& trackOrMaster); // empty for none
} // namespace phazite
