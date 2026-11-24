// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Tracks.h"
#include "internal/Clips.h"
#include "model/automation/internal/Automation.h"
#include "model/base/Toggles.h"
#include "model/base/internal/DeviceStoreValue.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongIndex.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/TrackSwitchesValue.h"
#include "model/base/internal/TracksOutside.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DrumKit.h"
#include "model/mixer/internal/Buses.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/modulation/internal/Modulation.h"
#include "model/notes/internal/Notes.h"
#include "model/plugins/internal/DeviceStore.h"
#include "model/versions/internal/AreaClashes.h"
#include "model/versions/internal/SongMerge.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    // Only a folder holds tracks: the others' children (their clips and devices, many in a big song) aren't looked through.
    void collectTrackStates (const juce::ValueTree& parent, std::vector<juce::ValueTree>& states)
    {
        SongIndex::countVisited ((std::uint64_t) parent.getNumChildren()); // a walk, for the speed gates

        for (const auto& child : parent)
        {
            if (child.hasType (schema::TRACK))
            {
                states.push_back (child);

                if (isFolderState (child))
                    collectTrackStates (child, states);
            }
        }
    }

    // A track's index in its folder's list (the top list for none), -1 for one the song hasn't; with a track of the list left
    // out (`without`: one that moves, as the list is once it is out of it).
    int findListIndex (const SongTracks& tracks, TrackID id, TrackID without = {})
    {
        const auto* value = tracks.tracks.find (id.getRaw());
        const auto* list = value != nullptr ? tracks.findList (value->parent) : nullptr;
        auto index = 0;

        if (list != nullptr)
        {
            for (const auto each : *list)
            {
                if (each == id)
                    return index;

                index += each != without ? 1 : 0;
            }
        }

        return -1;
    }

    // Whether a track is the folder given or in it, through its folders up.
    bool isInOrIs (const SongTracks& tracks, TrackID track, TrackID folder)
    {
        for (auto at = track; at.isValid();)
        {
            if (at == folder)
                return true;

            const auto* value = tracks.tracks.find (at.getRaw());
            at = value != nullptr ? value->parent : TrackID();
        }

        return false;
    }

    // A track moved to a place in a folder's list (setTrackPlace(): the same element), and where it came to another folder,
    // the sidechains that would hear themselves through it forgotten (into a bus that a sidechain on it listens to, say).
    void placeTrack (Song& song, TrackID track, TrackID folder, int index)
    {
        const auto held = findHeldTrack (song, track);

        if (setTrackPlace (song, track, folder, index) && held.has_value() && held->parent != folder)
            forgetSidechainLoops (song);
    }

    // The tracks that move, in the playlist's order: those given, but those inside a folder that moves, which go with it.
    std::vector<TrackID> getMovingTracks (const Song& song, const std::vector<TrackID>& tracks)
    {
        const auto held = getHeldTracks (song);
        const auto isGiven = [&tracks] (TrackID id) { return std::find (tracks.begin(), tracks.end(), id) != tracks.end(); };
        std::vector<TrackID> moving;

        for (const auto id : getTrackIDs (song))
        {
            if (! isGiven (id))
                continue;

            auto inMovingFolder = false;
            const auto* value = held.tracks.find (id.getRaw());

            for (auto folder = value != nullptr ? value->parent : TrackID(); folder.isValid() && ! inMovingFolder;)
            {
                inMovingFolder = isGiven (folder);
                const auto* parent = held.tracks.find (folder.getRaw());
                folder = parent != nullptr ? parent->parent : TrackID();
            }

            if (! inMovingFolder)
                moving.push_back (id);
        }

        return moving;
    }

    // Where moving tracks dropped at a track go, as the song is before they move: the folder whose list they go into (none for
    // the top list); into it, after its last element, or else before the first track of its list from the target on that
    // doesn't move itself (none: after the list's last). Nothing for a folder that would go into itself or a folder inside it.
    struct DropPlace
    {
        TrackID folder {}, before {};
        bool into = false;
    };

    std::optional<DropPlace> findDropPlace (const SongTracks& tracks, const std::vector<TrackID>& moving, TrackID target, TrackDrop drop)
    {
        const auto isMoving = [&moving] (TrackID id) { return std::find (moving.begin(), moving.end(), id) != moving.end(); };
        const auto* value = tracks.tracks.find (target.getRaw());

        if (value == nullptr)
            return {};

        DropPlace place;

        if (drop == TrackDrop::into)
        {
            if (! value->isFolder())
                return {};

            place.folder = target;
            place.into = true;
        }
        else
        {
            place.folder = value->parent;

            if (const auto* list = tracks.findList (place.folder))
                for (auto i = findListIndex (tracks, target) + (drop == TrackDrop::after ? 1 : 0);
                     i < (int) list->size() && ! place.before.isValid(); ++i)
                    if (const auto track = (*list)[(size_t) i]; ! isMoving (track))
                        place.before = track;
        }

        for (const auto each : moving)
            if (place.folder.isValid() && isInOrIs (tracks, place.folder, each))
                return {};

        return place;
    }

    // The tracks that have a row in the playlist, or can have one: its tracks and folders, and the returns and the master,
    // whose rows show while they have automation lanes.
    std::vector<TrackID> getRowTracks (const Song& song)
    {
        auto tracks = getTrackIDs (song);

        if (const auto master = getMasterID (song); master.isValid())
            tracks.push_back (master);

        return tracks;
    }

    // Moves a track right after another one, in its folder, or else first in the list.
    void moveTrackAfter (Song& song, TrackID track, TrackID previous)
    {
        const auto tracks = getHeldTracks (song);

        if (tracks.tracks.find (track.getRaw()) == nullptr || track == previous)
            return;

        if (const auto* before = tracks.tracks.find (previous.getRaw()))
            placeTrack (song, track, before->parent, findListIndex (tracks, previous, track) + 1);
        else
            placeTrack (song, track, {}, 0);
    }

    // The palette's next color after the track a new one goes below (the last track if that is none), so that neighbours
    // differ.
    juce::Colour getNextColour (const Song& song, TrackID after)
    {
        const auto tracks = getTrackIDs (song);
        const auto& colours = getTrackColours();
        size_t next = 0;

        if (const auto above = after.isValid() ? after : (tracks.empty() ? TrackID() : tracks.back()); above.isValid())
        {
            const auto row = (int) (std::find (tracks.begin(), tracks.end(), above) - tracks.begin());
            const auto aboveColour = getTrackColour (song, above, row);

            for (size_t i = 0; i < colours.size(); ++i)
                if (colours[i].colour == aboveColour)
                    next = (i + 1) % colours.size();
        }

        return colours[next].colour;
    }

    // The playlist track before (direction -1) or after (1) this one in its folder, or among the top-level tracks.
    TrackID getSibling (const Song& song, TrackID track, int direction)
    {
        const auto tracks = getHeldTracks (song);
        const auto* value = tracks.tracks.find (track.getRaw());
        const auto* list = value != nullptr ? tracks.findList (value->parent) : nullptr;

        if (const auto i = findListIndex (tracks, track) + direction; list != nullptr && juce::isPositiveAndBelow (i, (int) list->size()))
            return (*list)[(size_t) i];

        return {};
    }

    // Whether a track's switch is on (a look in the switches' map by its ID), or a folder's it is in (up its elements).
    bool isOnOrInFolderOn (const immer::map<std::uint64_t, bool>& switches, const juce::ValueTree& state)
    {
        for (auto element = state; element.hasType (schema::TRACK); element = element.getParent())
            if (switches.find (readTrackElementID (element).getRaw()) != nullptr)
                return true;

        return false;
    }

    bool isSoloedOrInSoloedFolder (const Song& song, const juce::ValueTree& state)
    {
        return isOnOrInFolderOn (getTrackSwitches (song).solos, state);
    }

    // The track whose drum sampler a pad's track plays a pad of (model/devices/internal/DrumKit.h), whose mute and solo cover
    // it as a folder's cover its tracks; none for any other track.
    juce::ValueTree findPadSamplerTrack (const Song& song, const juce::ValueTree& state)
    {
        if (! readPad (state).has_value())
            return {};

        const auto sampler = findTrackState (song, getDeviceChannel (song, readPadDevice (state)));
        return sampler != state ? sampler : juce::ValueTree();
    }

    bool isMutedOrInMutedFolder (const Song& song, const juce::ValueTree& state)
    {
        return isOnOrInFolderOn (getTrackSwitches (song).mutes, state);
    }

    // A channel's element: the master's by its flag, else its track's; invalid for none.
    juce::ValueTree findChannelState (const Song& song, ChannelRef channel)
    {
        if (channel.onMaster)
            return song.getState().getChildWithName (schema::MASTER);

        return channel.track.isValid() ? findTrackState (song, channel.track) : juce::ValueTree();
    }

    // The master's element, added with IDs of the song's for the master and its fader if the song has none yet, without an
    // undo step: what every song has (getTrackList()).
    void addMaster (Song& song)
    {
        auto phazite = song.getState();

        if (phazite.getChildWithName (schema::MASTER).isValid())
            return;

        juce::ValueTree master (schema::MASTER);
        master.setProperty (schema::trackID, (juce::int64) song.makeID(), nullptr);
        master.setProperty (schema::channelFader, (juce::int64) song.makeID(), nullptr);
        phazite.appendChild (master, nullptr);
    }

    // A new track's value, of a kind, with an ID of the song's (Song::makeID()) and its fader's (a folder starts as a bus:
    // design question 11), in a colour where it has one; and where it goes, after another track (in that one's folder), or
    // else at the end of the list: its folder, and its place in that folder's list.
    TrackValue makeTrack (Song& song, TrackValue::Kind kind, std::optional<juce::Colour> colour)
    {
        TrackValue value;
        value.id = TrackID::fromRaw (song.makeID());
        value.kind = kind;
        value.colour = colour.has_value() ? std::optional (colour->getARGB()) : std::nullopt;
        value.fader = DeviceID::fromRaw (song.makeID());
        return value;
    }

    int placeAfter (const Song& song, TrackValue& value, TrackID after)
    {
        const auto tracks = getHeldTracks (song);

        if (const auto* previous = tracks.tracks.find (after.getRaw()))
        {
            value.parent = previous->parent;
            return findListIndex (tracks, after) + 1;
        }

        value.parent = {};
        return -1;
    }
} // namespace

//==============================================================================
TrackLookup::TrackLookup (std::optional<TrackValue> found) noexcept
    : value (std::move (found))
{
}

const TrackValue* TrackLookup::operator->() const& noexcept
{
    jassert (value.has_value()); // a lookup that found none holds no track
    return &*value;
}

const TrackValue& TrackLookup::operator*() const& noexcept
{
    jassert (value.has_value()); // a lookup that found none holds no track
    return *value;
}

TrackLookup findTrack (const Song& song, TrackID id)
{
    return TrackLookup (readTrackValue (findTrackState (song, id)));
}

bool hasTrack (const Song& song, TrackID id)
{
    return findTrackState (song, id).isValid();
}

SongTracks getTracksValue (const Song& song)
{
    return readTracksValue (song.getState());
}

TrackOrder getTrackOrder (const Song& song)
{
    TrackOrder order;
    const auto& index = song.getIndex();

    for (const auto& state : index.getTracks())
    {
        const auto id = readTrackElementID (state);
        const auto place = index.getTrackPlace (state);
        TrackOrder::Place kept;
        kept.index = place.index;
        kept.number = place.number;
        order.places.try_emplace (id.getRaw(), kept);
        order.order.push_back (id);
    }

    return order;
}

DeviceID getChannelFader (const Song& song, ChannelRef channel)
{
    return readChannelFader (findChannelState (song, channel));
}

DeviceID getChannelVCA (const Song& song, ChannelRef channel)
{
    return readChannelVCA (findChannelState (song, channel));
}

bool isFaderOf (const Song& song, ChannelRef channel, DeviceID device)
{
    const auto state = findChannelState (song, channel);
    return device.isValid() && (readChannelFader (state) == device || readChannelVCA (state) == device);
}

TrackOrder::Place getTrackPlace (const Song& song, TrackID id)
{
    const auto state = findTrackState (song, id);

    if (! state.hasType (schema::TRACK))
        return {};

    const auto place = song.getIndex().getTrackPlace (state);
    TrackOrder::Place found;
    found.index = place.index;
    found.number = place.number;
    return found;
}

std::set<std::uint64_t> getAutomatableIDs (const Song& song, TrackID id)
{
    std::set<std::uint64_t> ids;
    const auto state = findTrackState (song, id);

    if (! state.isValid())
        return ids;

    for (const auto device : { readChannelFader (state), readChannelVCA (state) })
        if (device.isValid())
            ids.insert (device.getRaw());

    for (const auto& device : getDeviceOutline (song, id))
        ids.insert (device.id.getRaw());

    for (const auto send : getSends (song, id))
        ids.insert (send.getRaw());

    const auto onMaster = state.hasType (schema::MASTER);
    const auto modulation = getModulationValue (song);

    for (const auto& entry : modulation.modulators)
        if (const auto& place = entry.second.place; onMaster ? place.onMaster : ! place.onMaster && place.track == id)
            ids.insert (entry.first);

    return ids;
}

//==============================================================================
juce::ValueTree getTrackList (Song& song)
{
    addMaster (song);
    return song.getState().getOrCreateChildWithName (schema::TRACKS, nullptr);
}

TrackID getMasterID (const Song& song)
{
    return readTrackElementID (song.getState().getChildWithName (schema::MASTER));
}

juce::ValueTree findTrackState (const Song& song, TrackID id)
{
    if (! id.isValid())
        return {};

    // The master's is its own element, with its view (its lanes).
    if (const auto master = song.getState().getChildWithName (schema::MASTER); readTrackElementID (master) == id)
        return master;

    return song.getIndex().findTrack (id.getRaw());
}

std::vector<juce::ValueTree> getTrackStates (const Song& song)
{
    return song.getIndex().getTracks();
}

std::vector<TrackID> getTrackIDs (const Song& song)
{
    std::vector<TrackID> ids;

    for (const auto& state : getTrackStates (song))
        ids.push_back (getTrackID (state));

    return ids;
}

TrackID getTrackID (const juce::ValueTree& state)
{
    return readTrackElementID (state);
}

bool isFolderState (const juce::ValueTree& state)
{
    return readTrackKind (state) == TrackValue::Kind::folder;
}

bool isReturnState (const juce::ValueTree& state)
{
    return readTrackKind (state) == TrackValue::Kind::returnTrack;
}

std::optional<TrackValue::Kind> getTrackKind (const Song& song, TrackID id)
{
    return readTrackKind (findTrackState (song, id));
}

std::vector<TrackID> getTrackIDsOfKind (const Song& song, TrackValue::Kind kind)
{
    std::vector<TrackID> ids;

    for (const auto& state : getTrackStates (song))
        if (readTrackKind (state) == kind)
            ids.push_back (readTrackElementID (state));

    return ids;
}

juce::String getTrackInput (const Song& song, TrackID id)
{
    return readTrackInput (findTrackState (song, id));
}

TrackValue::Monitoring getTrackMonitoring (const Song& song, TrackID id)
{
    return readTrackMonitoring (findTrackState (song, id));
}

bool isFolder (const Song& song, TrackID id)
{
    return isFolderState (findTrackState (song, id));
}

bool isReturnTrack (const Song& song, TrackID id)
{
    return isReturnState (findTrackState (song, id));
}

bool isMaster (const Song& song, TrackID id)
{
    return id.isValid() && id == getMasterID (song);
}

juce::String getTrackName (const Song& song, TrackID id)
{
    const auto state = findTrackState (song, id);
    const auto kind = readTrackKind (state);

    if (! kind.has_value() || *kind == TrackValue::Kind::master)
        return kind.has_value() ? juce::String ("Master") : juce::String();

    if (const auto name = readTrackName (state); name.isNotEmpty() || *kind == TrackValue::Kind::folder)
        return name;

    // Its number among the tracks that aren't folders, as the song's index keeps it: every track findTrackState() finds has
    // one.
    const auto number = song.getIndex().getTrackPlace (state).number;
    jassert (number > 0);
    return number > 0 ? "Track " + juce::String (number) : juce::String ("Track");
}

TrackID getParentFolder (const Song& song, TrackID id)
{
    if (const auto parent = findTrackState (song, id).getParent(); parent.hasType (schema::TRACK))
        return getTrackID (parent);

    return {};
}

std::vector<TrackID> getTracksInFolder (const Song& song, TrackID folder)
{
    std::vector<juce::ValueTree> states;
    collectTrackStates (findTrackState (song, folder), states);

    std::vector<TrackID> ids;

    for (const auto& state : states)
        ids.push_back (getTrackID (state));

    return ids;
}

bool isTrackMuted (const Song& song, TrackID id, bool orInMutedFolder)
{
    const auto state = findTrackState (song, id);

    if (! orInMutedFolder)
        return state.hasType (schema::TRACK) && getTrackSwitches (song).mutes.find (id.getRaw()) != nullptr;

    // A pad's track is muted with its sampler's track, as a track is with its folder.
    return isMutedOrInMutedFolder (song, state) || isMutedOrInMutedFolder (song, findPadSamplerTrack (song, state));
}

bool isTrackSoloed (const Song& song, TrackID id)
{
    return getTrackSwitches (song).solos.find (id.getRaw()) != nullptr && findTrackState (song, id).hasType (schema::TRACK);
}

bool areAnyTracksSoloed (const Song& song)
{
    // Each solo the switches hold, of a track the song holds (one a step took away keeps its entry, for an Undo).
    const auto solos = getTrackSwitches (song).solos;
    return std::any_of (solos.begin(), solos.end(), [&song] (const auto& entry)
                        { return findTrackState (song, TrackID::fromRaw (entry.first)).hasType (schema::TRACK); });
}

bool isTrackFrozen (const Song& song, TrackID id)
{
    return getTrackSwitches (song).frozen.find (id.getRaw()) != nullptr && findTrackState (song, id).hasType (schema::TRACK);
}

std::optional<TrackPad> getTrackPad (const Song& song, TrackID id)
{
    const auto state = findTrackState (song, id);

    if (const auto pad = readPad (state))
        return TrackPad { .track = id, .sampler = readPadDevice (state), .pad = *pad };

    return std::nullopt;
}

std::vector<TrackPad> getPadTracks (const Song& song)
{
    std::vector<TrackPad> pads;

    for (const auto& state : getTrackStates (song))
        if (const auto pad = readPad (state))
            pads.push_back ({ .track = readTrackElementID (state), .sampler = readPadDevice (state), .pad = *pad });

    return pads;
}

void setTrackMuted (Song& song, TrackID id, bool muted, juce::UndoManager* undo)
{
    // Off is no entry at all, as it is at first: unmuted again, the track is as it was, which a version's changes then don't
    // count (docs/design/versions.md). One entry set, the switches' setter asked only where it differs.
    if (findTrackState (song, id).hasType (schema::TRACK))
        setTrackSwitches (song, withMute (getTrackSwitches (song), id.getRaw(), muted), undo != nullptr);
}

void setTrackSoloed (Song& song, TrackID id, bool soloed)
{
    if (findTrackState (song, id).hasType (schema::TRACK))
        setTrackSwitches (song, withSolo (getTrackSwitches (song), id.getRaw(), soloed), false);
}

void setTrackName (Song& song, TrackID id, const juce::String& name)
{
    if (! isMaster (song, id))
        setTrackFields (song, id, [&name] (TrackValue& value) { value.name = name; });
}

void setTrackColour (Song& song, TrackID id, juce::Colour colour)
{
    // A transparent colour is none (TrackValue::colour): the track takes its row's in the palette, as every reader read it.
    if (! isMaster (song, id))
        setTrackFields (song, id, [colour] (TrackValue& value)
                        { value.colour = colour.isTransparent() ? std::nullopt : std::optional (colour.getARGB()); });
}

TrackID insertPlainTrack (Song& song, TrackID after)
{
    auto value = makeTrack (song, TrackValue::Kind::track, std::nullopt);
    const auto index = placeAfter (song, value, after);
    return setTrack (song, value, index) ? value.id : TrackID();
}

//==============================================================================
juce::Colour getTrackColour (const Song& song, TrackID id, int row)
{
    return getTrackColour (readTrackColour (findTrackState (song, id)), row);
}

juce::Colour getTrackColour (std::optional<juce::uint32> own, int row)
{
    if (own.has_value())
        return juce::Colour (*own);

    const auto& colours = getTrackColours();
    return colours[(size_t) (std::max (0, row) % (int) colours.size())].colour;
}

//==============================================================================
TrackView getTrackView (const Song& song, TrackID id)
{
    return getViewOf (song, findTrackState (song, id));
}

TrackView getViewOf (const Song& song, const juce::ValueTree& state)
{
    // A song that holds no values (a check's copy): its tree holds the view, read as Phazite's code always read it.
    const auto* values = song.getValues();

    if (values == nullptr)
        return readViewOf (state);

    if (state.hasType (schema::MASTER))
        return values->tracksOutside.get().master;

    const auto* view =
        state.hasType (schema::TRACK) ? values->tracksOutside.get().tracks.find (readTrackElementID (state).getRaw()) : nullptr;
    return view != nullptr ? *view : TrackView();
}

TrackSwitches getTrackSwitches (const Song& song)
{
    const auto* values = song.getValues();
    return values != nullptr ? values->trackSwitches.get() : readTrackSwitches (song.getState());
}

TracksOutside getTracksOutside (const Song& song)
{
    const auto* values = song.getValues();
    return values != nullptr ? values->tracksOutside.get() : readTracksOutside (song.getState());
}

std::optional<std::uint64_t> findSwitchKey (const Song& song, TrackID id)
{
    const auto state = findTrackState (song, id);

    if (state.hasType (schema::MASTER))
        return (std::uint64_t) 0;

    return state.hasType (schema::TRACK) ? std::optional (id.getRaw()) : std::nullopt;
}

void setTrackSwitches (Song& song, TrackSwitches next, bool inHistory)
{
    if (auto* values = song.getValues())
    {
        if (inHistory)
            values->setSwitches (song, std::move (next));
        else
            values->setSwitchesOutsideHistory (song, std::move (next));
    }
}

void setTrackView (Song& song, TrackID id, TrackView view)
{
    auto* values = song.getValues();
    const auto key = findSwitchKey (song, id);

    if (values == nullptr || ! key.has_value())
        return;

    const auto& outside = values->tracksOutside.get();
    values->setTracksOutside (*key == 0 ? withMasterView (outside, std::move (view)) : withTrackView (outside, *key, std::move (view)));
}

void setTrackFrozen (Song& song, TrackID id, bool frozen, juce::UndoManager* undo)
{
    if (findTrackState (song, id).hasType (schema::TRACK))
        setTrackSwitches (song, withFrozen (getTrackSwitches (song), id.getRaw(), frozen), undo != nullptr);
}

std::optional<int> getSavedTrackHeight (const Song& song, TrackID id)
{
    return getTrackView (song, id).height;
}

void setSavedTrackHeight (Song& song, TrackID id, int height)
{
    // One row's view set, a look in a map: a drag's event of a row's height reads no track and compares no other row.
    if (auto view = getTrackView (song, id); view.height != height)
    {
        view.height = height;
        setTrackView (song, id, std::move (view));
    }
}

bool isCollapsed (const Song& song, TrackID id)
{
    return getTrackView (song, id).collapsed.value_or (false);
}

void setCollapsed (Song& song, TrackID id, bool collapsed)
{
    // Off as false, as Phazite always wrote it (the golden command script holds collapsed="0": TracksOutside.h).
    const auto wanted = std::optional (collapsed);

    if (auto view = getTrackView (song, id); view.collapsed != wanted)
    {
        view.collapsed = wanted;
        setTrackView (song, id, std::move (view));
    }
}

bool isPinned (const Song& song, TrackID id)
{
    return getTrackView (song, id).pinned.value_or (false);
}

void setPinned (Song& song, TrackID id, bool pinned)
{
    const auto wanted = pinned ? std::optional (true) : std::nullopt;

    if (auto view = getTrackView (song, id); view.pinned != wanted)
    {
        view.pinned = wanted;
        setTrackView (song, id, std::move (view));
    }
}

bool isHiddenInCollapsedFolder (const Song& song, TrackID id)
{
    for (auto folder = findTrackState (song, id).getParent(); folder.hasType (schema::TRACK); folder = folder.getParent())
        if (getViewOf (song, folder).collapsed.value_or (false))
            return true;

    return false;
}

void toggleSoloAlone (Song& song, TrackID id)
{
    if (! findTrackState (song, id).hasType (schema::TRACK))
        return;

    // Every other track's solo off (of the tracks the song holds: a track a step took away keeps its entry), and its own on,
    // or off where it was soloed alone: one set of the whole next value, one call to the listeners.
    auto next = getTrackSwitches (song);
    auto alone = isTrackSoloed (song, id);
    auto solos = next.solos;

    for (const auto& entry : next.solos)
    {
        if (entry.first == id.getRaw() || ! findTrackState (song, TrackID::fromRaw (entry.first)).hasType (schema::TRACK))
            continue;

        alone = false;
        solos = solos.erase (entry.first);
    }

    next.solos = alone ? solos.erase (id.getRaw()) : solos.set (id.getRaw(), true);
    setTrackSwitches (song, std::move (next), false);
}

bool isSilencedBySolo (const Song& song, TrackID id)
{
    const auto state = findTrackState (song, id);
    return areAnyTracksSoloed (song) && ! isSoloedOrInSoloedFolder (song, state)
           && ! isSoloedOrInSoloedFolder (song, findPadSamplerTrack (song, state)) && ! isReturnState (state)
           && ! isTrackMuted (song, id, false);
}

//==============================================================================
TrackID insertTrack (Song& song, TrackID after, Selection* selection)
{
    auto value = makeTrack (song, TrackValue::Kind::track, getNextColour (song, after));
    value.key = getLastKey (song); // the key chosen last in the song, in the step that adds it
    const auto index = placeAfter (song, value, after);
    const auto id = setTrack (song, value, index) ? value.id : TrackID();

    if (selection != nullptr)
        selection->selectOnly (id);

    return id;
}

TrackID insertReturnTrack (Song& song, const juce::String& name, Selection* selection)
{
    auto value = makeTrack (song, TrackValue::Kind::returnTrack, getNextColour (song, {}));
    value.name = name;
    value.key = getLastKey (song); // as any new track gets it
    const auto id = setTrack (song, value) ? value.id : TrackID();

    if (selection != nullptr)
        selection->selectOnly (id);

    return id;
}

std::vector<TrackID> ensureTracks (Song& song, int count)
{
    const auto getTracks = [&song] { return getTrackIDsOfKind (song, TrackValue::Kind::track); };

    for (auto tracks = getTracks(); (int) tracks.size() < count; tracks = getTracks())
        insertPlainTrack (song, {});

    return getTracks();
}

TrackID insertFolder (Song& song, TrackID after, Selection* selection)
{
    auto value = makeTrack (song, TrackValue::Kind::folder, getNextColour (song, after));
    value.name = juce::String ("Folder");
    value.bus = true; // a bus from the start (design question 11): its fader its own (Buses.h: mixAsBus())
    const auto index = placeAfter (song, value, after);
    const auto folder = setTrack (song, value, index) ? value.id : TrackID();

    if (selection != nullptr)
        selection->selectOnly (folder);

    return folder;
}

TrackID groupIntoFolder (Song& song, const std::vector<TrackID>& tracks, Selection* selection)
{
    const auto held = getHeldTracks (song);
    const auto* first = tracks.empty() ? nullptr : held.tracks.find (tracks.front().getRaw());

    if (first == nullptr)
        return {};

    // Where the first track is, in its own folder if it has one; in its color; a bus from the start (design question 11).
    const auto all = getTrackIDs (song);
    const auto row = (int) (std::find (all.begin(), all.end(), tracks.front()) - all.begin());
    auto value = makeTrack (song, TrackValue::Kind::folder, getTrackColour (song, tracks.front(), row));
    value.name = juce::String ("Folder");
    value.bus = true;
    value.parent = first->parent;

    if (! setTrack (song, value, findListIndex (held, tracks.front())))
        return {};

    const auto folder = value.id;

    for (const auto track : tracks)
        moveTrackIntoFolder (song, track, folder);

    if (selection != nullptr)
        selection->selectOnly (folder);

    return folder;
}

void moveTrackIntoFolder (Song& song, TrackID track, TrackID folder)
{
    // A folder can't go into itself, or into a folder inside it; after the folder's last element.
    const auto held = getHeldTracks (song);
    const auto* into = held.tracks.find (folder.getRaw());

    if (held.tracks.find (track.getRaw()) == nullptr || into == nullptr || ! into->isFolder() || isInOrIs (held, folder, track))
        return;

    placeTrack (song, track, folder, -1);
}

void ungroupFolder (Song& song, TrackID folder)
{
    const auto held = getHeldTracks (song);
    const auto* value = held.tracks.find (folder.getRaw());

    if (value == nullptr || ! value->isFolder())
        return;

    // What its bus's devices hold of their own stored first (a plug-in's own settings), so that an undo brings it back as it is.
    song.storeDeviceStates (folder.getRaw());

    // Its tracks go where it was, in their order, and it goes, with the sidechains that listened to it.
    forgetSidechainsOfBus (song, folder);
    const auto parent = value->parent;
    auto place = findListIndex (held, folder) + 1;

    if (const auto* list = held.findList (folder))
        for (const auto track : *list)
            placeTrack (song, track, parent, place++);

    // What it holds itself (a bus's modulators and sends) erased before it goes; its tracks are out of it now.
    eraseTrackHoldings (song, { folder });
    eraseTrack (song, folder);

    // Then what named its fader or VCA and its bus's devices, which went with it: their curves, and the modulations and the
    // MIDI links of them (from modulators and links elsewhere), so that nothing is left naming what is gone.
    forgetWhatNamesTheGone (song);
}

//==============================================================================
std::vector<TrackID> getSelectedTracks (const Selection& selection)
{
    std::vector<TrackID> selected;

    if (auto* song = selection.getSong())
        for (const auto id : getTrackIDs (*song))
            if (selection.isSelected (id))
                selected.push_back (id);

    return selected;
}

std::vector<TrackID> getSelectedOutermostTracks (const Selection& selection)
{
    std::vector<TrackID> tracks;
    auto* song = selection.getSong();

    for (const auto id : getSelectedTracks (selection))
    {
        auto inSelectedFolder = false;

        for (auto folder = getParentFolder (*song, id); folder.isValid() && ! inSelectedFolder; folder = getParentFolder (*song, folder))
            inSelectedFolder = selection.isSelected (folder);

        if (! inSelectedFolder)
            tracks.push_back (id);
    }

    return tracks;
}

bool hasSelectedTracks (const Selection& selection)
{
    return ! getSelectedTracks (selection).empty();
}

std::vector<TrackID> getSelectedFolders (const Selection& selection)
{
    std::vector<TrackID> folders;

    for (const auto id : getSelectedTracks (selection))
        if (isFolder (*selection.getSong(), id))
            folders.push_back (id);

    return folders;
}

bool hasSelectedFolders (const Selection& selection)
{
    return ! getSelectedFolders (selection).empty();
}

std::vector<TrackID> getSelectedTracksOrAll (const Selection& selection)
{
    auto tracks = getSelectedTracks (selection);

    if (tracks.empty())
        if (auto* song = selection.getSong())
            tracks = getTrackIDs (*song);

    return tracks;
}

TrackID insertTrackAfterSelection (Selection& selection)
{
    auto* song = selection.getSong();
    const auto selected = getSelectedTracks (selection);
    return song != nullptr ? insertTrack (*song, selected.empty() ? TrackID() : selected.back(), &selection) : TrackID();
}

TrackID insertFolderAfterSelection (Selection& selection)
{
    auto* song = selection.getSong();
    const auto selected = getSelectedTracks (selection);
    return song != nullptr ? insertFolder (*song, selected.empty() ? TrackID() : selected.back(), &selection) : TrackID();
}

void ungroupFolders (Song& song, const std::vector<TrackID>& folders)
{
    for (const auto folder : folders)
        ungroupFolder (song, folder);
}

void selectNeighbourTrack (Selection& selection, int direction)
{
    auto* song = selection.getSong();

    if (song == nullptr)
        return;

    const auto tracks = getTrackIDs (*song);
    const auto selected = getSelectedTracks (selection);

    if (tracks.empty())
        return;

    if (selected.empty())
    {
        selection.selectOnly (direction > 0 ? tracks.front() : tracks.back());
        return;
    }

    const auto from = std::find (tracks.begin(), tracks.end(), direction > 0 ? selected.back() : selected.front());
    const auto row = (int) (from - tracks.begin()) + direction;
    selection.selectOnly (tracks[(size_t) juce::jlimit (0, (int) tracks.size() - 1, row)]);
}

void moveSelectedTracks (Selection& selection, int direction)
{
    auto* song = selection.getSong();

    if (song == nullptr)
        return;

    auto tracks = getSelectedOutermostTracks (selection);

    // The track nearest to where they move goes first.
    if (direction > 0)
        std::reverse (tracks.begin(), tracks.end());

    for (const auto track : tracks)
    {
        const auto neighbour = getSibling (*song, track, direction);

        if (! neighbour.isValid() || selection.isSelected (neighbour))
            continue;

        if (direction < 0)
            moveTrackAfter (*song, neighbour, track); // the one above goes below
        else
            moveTrackAfter (*song, track, neighbour);
    }
}

bool areAllTracksMuted (const Song& song, const std::vector<TrackID>& tracks)
{
    return areAllOn (tracks, [&song] (TrackID id) { return isTrackMuted (song, id, false); });
}

void toggleTracksMuted (Song& song, const std::vector<TrackID>& tracks)
{
    toggleAll (
        tracks, [&song] (TrackID id) { return isTrackMuted (song, id, false); },
        [&song] (TrackID id, bool on) { setTrackMuted (song, id, on); });
}

bool areAllTracksSoloed (const Song& song, const std::vector<TrackID>& tracks)
{
    return areAllOn (tracks, [&song] (TrackID id) { return isTrackSoloed (song, id); });
}

void toggleTracksSoloed (Song& song, const std::vector<TrackID>& tracks)
{
    toggleAll (
        tracks, [&song] (TrackID id) { return isTrackSoloed (song, id); },
        [&song] (TrackID id, bool on) { setTrackSoloed (song, id, on); });
}

bool areAllTracksCollapsed (const Song& song, const std::vector<TrackID>& tracks)
{
    return areAllOn (tracks, [&song] (TrackID id) { return isCollapsed (song, id); });
}

void toggleTracksCollapsed (Song& song, const std::vector<TrackID>& tracks)
{
    toggleAll (
        tracks, [&song] (TrackID id) { return isCollapsed (song, id); }, [&song] (TrackID id, bool on) { setCollapsed (song, id, on); });
}

bool areAllTracksPinned (const Song& song, const std::vector<TrackID>& tracks)
{
    return areAllOn (tracks, [&song] (TrackID id) { return isPinned (song, id); });
}

void toggleTracksPinned (Song& song, const std::vector<TrackID>& tracks)
{
    toggleAll (tracks, [&song] (TrackID id) { return isPinned (song, id); }, [&song] (TrackID id, bool on) { setPinned (song, id, on); });
}

bool isAnyRowExpanded (const Song& song)
{
    for (const auto track : getRowTracks (song))
    {
        // A return's or the master's own row shows only with lanes.
        const auto lanes = getLanes (song, track);
        const auto hasRow = ! isMaster (song, track) && ! isReturnTrack (song, track);

        if ((hasRow || ! lanes.empty()) && ! isCollapsed (song, track))
            return true;

        if (std::any_of (lanes.begin(), lanes.end(), [] (const AutomationLane& lane) { return ! lane.collapsed; }))
            return true;
    }

    return false;
}

void setAllRowsCollapsed (Song& song, bool collapsed)
{
    for (const auto track : getRowTracks (song))
    {
        setCollapsed (song, track, collapsed);

        for (const auto& lane : getLanes (song, track))
            setLaneCollapsed (song, track, lane.parameter, collapsed);
    }
}

void toggleAllRowsCollapsed (Song& song)
{
    setAllRowsCollapsed (song, isAnyRowExpanded (song));
}

namespace
{
    // Each copied track given its original's switches, the frozen one aside (a copy plays live: the file is the original's),
    // and its view, its lanes naming its devices' copies (copied: a lane's device by its copy's ID, design K.12), before the
    // copy comes into the song: by the pairs, each original with its copy. The switches written outside the history, as a
    // maker gives a new item's entries in a keyed area (model/plugins/DeviceStore.cpp: setMadeStore()): the copies' IDs are
    // new (the song's count of IDs is never undone), so the area keeps none of them; the step's Undo takes the copy away,
    // leaving its entries unseen, and what is written for it since outside the history (the copy unmuted) is what its Redo
    // brings it back with, as its element carried its mute while the tree held the switches. Written in the step, the Redo
    // would put the copied mute back over that. The view is never in the history.
    void copyTrackLeaves (Song& song, const std::vector<std::pair<TrackID, TrackID>>& originalsAndCopies, const CopiedIDs& copied)
    {
        auto* values = song.getValues();

        if (values == nullptr)
            return;

        const auto held = values->trackSwitches.get();
        auto switches = held;
        auto outside = values->tracksOutside.get();

        for (const auto& [originalID, copyID] : originalsAndCopies)
        {
            const auto old = originalID.getRaw();
            const auto made = copyID.getRaw();
            switches = withMute (switches, made, held.mutes.find (old) != nullptr);
            switches = withSolo (switches, made, held.solos.find (old) != nullptr);
            const auto* mode = held.modes.find (old);
            switches = withAutomationMode (switches, made, mode != nullptr ? std::optional (*mode) : std::nullopt);

            if (const auto* view = values->tracksOutside.get().tracks.find (old))
            {
                auto copiedView = *view;
                copiedView.lanes = {};

                for (auto lane : view->lanes)
                {
                    if (const auto found = copied.find (lane.device.getRaw()); found != copied.end())
                        lane.device = DeviceID::fromRaw (found->second);

                    copiedView.lanes = copiedView.lanes.push_back (std::move (lane));
                }

                outside = withTrackView (outside, made, std::move (copiedView));
            }
        }

        values->setTracksOutside (std::move (outside));
        values->setSwitchesOutsideHistory (song, std::move (switches));
    }
} // namespace

std::vector<TrackID> duplicateTracks (Song& song, const std::vector<TrackID>& tracks, Selection* selection)
{
    std::vector<TrackID> copies;

    for (const auto track : tracks)
    {
        if (const auto held = findHeldTrack (song, track); ! held.has_value() || held->isMaster())
            continue;

        // The copy holds what its devices hold now (a plug-in's own settings), every item in it the song's with an ID of its
        // own (its tracks, devices, sends, modulators and clips, its clips' notes as they are, in their alias groups), with
        // its own automation, right after the original: the tracks as values with new IDs (makeTrackCopies()), the copy of a
        // folder with the tracks in it, put in as one (setNewTracks()). A frozen track's copy plays live: the frozen file is the
        // original's.
        song.storeDeviceStates (track.getRaw());
        CopiedIDs copied;
        const auto made = makeTrackCopies (song, track, copied);

        if (made.values.empty())
            continue;

        // Each copied track's switches and view given before the copies come into the song (copyTrackLeaves(): by each
        // original with its copy; its lanes name its devices' copies, which makeTrackCopies() gave into `copied`), and put
        // back as they were if the song refuses the copies, so that no entry stays for a track it never took.
        auto* values = song.getValues();
        const auto switchesBefore = values != nullptr ? values->trackSwitches.get() : TrackSwitches {};
        const auto outsideBefore = values != nullptr ? values->tracksOutside.get() : TracksOutside {};
        copyTrackLeaves (song, made.originalsAndCopies, copied);

        if (! setNewTracks (song, made.values, findListIndex (getHeldTracks (song), track) + 1))
        {
            if (values != nullptr)
            {
                values->setTracksOutside (outsideBefore);
                values->setSwitchesOutsideHistory (song, switchesBefore);
            }

            continue;
        }

        // Each track's clips onto its copy, in their alias groups, from the original by the copy's ID.
        for (const auto& [original, copy] : made.originalsAndCopies)
            copyTrackClips (song, original, copy, copied, true);

        // Its devices, through the devices' code; then its sends, through the sends' code, then its modulators and its
        // containers' macros, through modulation's code, each with their new IDs among the copied before what names them is
        // copied: a modulation of a send's level names the send's copy by its ID, and a curve a send's copy or a macro's.
        copyDevices (song, copied);
        copySends (song, copied);
        copyModulation (song, copied);
        copyCurves (song, copied);
        copies.push_back (made.values.front().id);
    }

    if (selection != nullptr)
    {
        selection->deselectAll();

        for (const auto copy : copies)
            selection->addToSelection (copy);
    }

    return copies;
}

void renameTrack (Song& song, TrackID track, const juce::String& name)
{
    const auto trimmed = name.trim();
    const auto held = findHeldTrack (song, track);

    if (! held.has_value() || held->isMaster() || trimmed == held->getOwnName())
        return;

    const UndoStep step (song, "Rename Track");
    setTrackFields (song, track, [&trimmed] (TrackValue& value) { value.name = trimmed; });
}

TrackID addTrackAtEnd (Song& song, bool folder, Selection* selection)
{
    const UndoStep step (song, folder ? "Add Folder" : "Add Track");

    if (folder)
        return insertFolder (song, {}, selection);

    return insertTrack (song, {}, selection);
}

void setTrackColours (Song& song, const std::vector<TrackID>& tracks, juce::Colour colour)
{
    if (tracks.empty())
        return;

    const UndoStep step (song, tracks.size() > 1 ? "Change Track Colors" : "Change Track Color");

    for (const auto track : tracks)
        setTrackColour (song, track, colour);
}

void moveTracksAfter (Song& song, const std::vector<TrackID>& moving, TrackID previous)
{
    if (moving.empty())
        return;

    const UndoStep step (song, moving.size() > 1 ? "Move Tracks" : "Move Track");

    for (const auto track : moving)
    {
        moveTrackAfter (song, track, previous);
        previous = track;
    }
}

bool canMoveTracksTo (const Song& song, const std::vector<TrackID>& tracks, TrackID target, TrackDrop drop)
{
    const auto moving = getMovingTracks (song, tracks);
    return ! moving.empty() && findDropPlace (getHeldTracks (song), moving, target, drop).has_value();
}

void moveTracksTo (Song& song, const std::vector<TrackID>& tracks, TrackID target, TrackDrop drop)
{
    const auto moving = getMovingTracks (song, tracks);
    const auto place = moving.empty() ? std::nullopt : findDropPlace (getHeldTracks (song), moving, target, drop);

    if (! place.has_value())
        return;

    const UndoStep step (song, moving.size() > 1 ? "Move Tracks" : "Move Track");

    // Each goes before the same track, which doesn't move, so that they keep their order; else each after the list's last
    // track (into a folder: after its last element).
    for (const auto track : moving)
    {
        const auto held = getHeldTracks (song);

        if (place->into)
            placeTrack (song, track, place->folder, -1);
        else if (place->before.isValid())
            placeTrack (song, track, place->folder, findListIndex (held, place->before, track));
        else if (const auto* list = held.findList (place->folder))
            placeTrack (song, track, place->folder,
                        (int) list->size() - (std::find (list->begin(), list->end(), track) != list->end() ? 1 : 0));
        else
            placeTrack (song, track, place->folder, 0);
    }
}

void deleteTracks (Song& song, const std::vector<TrackID>& tracks, Selection* selection)
{
    // A folder takes the tracks inside it along, so those aren't deleted on their own.
    std::vector<TrackID> outermost;

    const auto held = getHeldTracks (song);

    for (const auto track : tracks)
    {
        if (held.tracks.find (track.getRaw()) == nullptr)
            continue;

        auto inDeletedFolder = false;

        for (auto folder = getParentFolder (song, track); folder.isValid() && ! inDeletedFolder; folder = getParentFolder (song, folder))
            inDeletedFolder = std::find (tracks.begin(), tracks.end(), folder) != tracks.end();

        if (! inDeletedFolder)
            outermost.push_back (track);
    }

    if (outermost.empty())
        return;

    // What their devices hold of their own stored first (a plug-in's own settings), so that an undo brings it back as it is.
    for (const auto track : outermost)
        song.storeDeviceStates (track.getRaw());

    const auto all = getTrackIDs (song);
    const auto row = (int) (std::find (all.begin(), all.end(), outermost.front()) - all.begin());
    forgetSidechainSources (song, outermost);
    eraseTrackHoldings (song, outermost); // what they hold in the song's areas, before their elements go

    for (const auto track : outermost)
        eraseTrack (song, track);

    unlinkLoneClips (song);        // a clip linked only to clips of the tracks gone
    forgetWhatNamesTheGone (song); // a send to a return gone, a pad's track whose sampler went with its track

    if (selection != nullptr)
        if (const auto remaining = getTrackIDs (song); ! remaining.empty())
            selection->selectOnly (remaining[(size_t) juce::jmin (row, (int) remaining.size() - 1)]);
}

void eraseTrackHoldings (Song& song, const std::vector<TrackID>& tracks)
{
    if (song.getValues() == nullptr || tracks.empty())
        return;

    // Each track once, with the tracks in each folder among them; and each channel's place, with its containers': a
    // container's macros are listed by its ID (ModulationValue.h: SongModulation), each keeping its channel in its place, by
    // which they are found (a container a version's pass took away first leaves its macros to the modulation pass after it).
    std::vector<TrackID> held, channels;
    std::vector<ModulatorPlace> owners;
    std::set<std::uint64_t> seen;
    const auto modulation = song.getValues()->modulation.get(); // a copy, which shares what it holds

    const auto addChannel = [&modulation, &owners, &channels] (TrackID id, const ModulatorPlace& channel)
    {
        owners.push_back (channel);
        channels.push_back (id);

        for (const auto& [container, list] : modulation.containers)
        {
            if (const auto* first = list.empty() ? nullptr : modulation.find (list.front());
                first != nullptr && first->place.onMaster == channel.onMaster && first->place.track == channel.track)
            {
                auto owner = channel;
                owner.container = DeviceID::fromRaw (container);
                owners.push_back (owner);
            }
        }
    };

    const auto addTrack = [&held, &seen, &addChannel] (TrackID id)
    {
        if (! seen.insert (id.getRaw()).second)
            return;

        held.push_back (id);
        addChannel (id, ModulatorPlace { .track = id });
    };

    for (const auto id : tracks)
    {
        if (isMaster (song, id))
        {
            if (seen.insert (id.getRaw()).second)
                addChannel (id, ModulatorPlace { .onMaster = true });

            continue;
        }

        if (! findTrackState (song, id).hasType (schema::TRACK))
            continue;

        addTrack (id);

        if (isFolder (song, id))
            for (const auto inside : getTracksInFolder (song, id))
                addTrack (inside);
    }

    // Each area through its own code, a line each, each set once: the clips with their notes, then the devices (each channel's
    // devices, containers and chains, the master's too: Mixer.h, eraseChannelDevices(); what the store holds of them stays),
    // then modulation (a container's macros by the places found above), then the sends.
    eraseTrackClips (song, held);
    eraseChannelDevices (song, channels);
    eraseModulatorsOf (song, owners);
    eraseTrackSends (song, held);
}

namespace
{
    // The tracks of a TRACK element, itself first and then those in it, depth first, as the keeper's parts walk one that comes
    // into the song (model/base/ClipsArea.cpp, SendsArea.cpp: collectTracks()).
    void collectTrackElements (const juce::ValueTree& element, std::vector<juce::ValueTree>& tracks)
    {
        if (! element.hasType (schema::TRACK))
            return;

        tracks.push_back (element);

        for (const auto& child : element)
            collectTrackElements (child, tracks);
    }

    // The owners of modulators and macros in a channel's element, as modulation's part of the keeper walks one that comes into
    // the song (model/base/ModulationArea.cpp: childAdded()): the channel, then each of its containers and, a folder's, each
    // of its tracks with theirs, in its elements' order.
    void collectModulationOwners (const juce::ValueTree& owner, std::vector<juce::ValueTree>& owners)
    {
        owners.push_back (owner);

        if (owner.hasType (schema::CONTAINER))
            return;

        for (const auto& child : owner)
            if (child.hasType (schema::CONTAINER)
                || (child.hasType (schema::TRACK) && owner.hasType (schema::TRACK) && isFolderState (owner)))
                collectModulationOwners (child, owners);
    }

    // Each DEVICE and CHAIN in an element, through what can hold one (a track, a folder's tracks, the master, a container, a
    // chain), in the tree's order, as the store's part of the keeper walks one that comes into the song (DeviceStoreArea.cpp).
    void collectDeviceItems (const juce::ValueTree& element, std::vector<juce::ValueTree>& items)
    {
        if (element.hasType (schema::DEVICE) || element.hasType (schema::CHAIN))
            items.push_back (element);

        if (element.hasType (schema::TRACK) || element.hasType (schema::MASTER) || element.hasType (schema::CONTAINER)
            || element.hasType (schema::CHAIN))
            for (const auto& child : element)
                collectDeviceItems (child, items);
    }

    // An element's children of two types taken out of it, outside any history (a copy's, before it joins the song).
    void removeChildrenOf (juce::ValueTree element, const juce::Identifier& first, const juce::Identifier& second)
    {
        for (int i = element.getNumChildren(); --i >= 0;)
            if (const auto child = element.getChild (i); child.hasType (first) || child.hasType (second))
                element.removeChild (i, nullptr);
    }
} // namespace

AreaChoice chooseTrackClash (Song& song, const MergeClash& clash, int value, const MergeSides&)
{
    // A switch of a track or the master's mode ("TRACK:12" and its mute): the switches' entry of its key given the value
    // chosen, or taken away where that side had none, in the choice's step, as the conversion reads it (TrackSwitchesValue.h:
    // readHeldSwitchesOf(), of the element with the song's other entries of it and the value chosen). The view counts nothing,
    // and a merge reports no clash of it.
    if (clash.property.isValid())
    {
        const auto element = findByPath (song.getState(), clash.path);

        if (! isSwitchProperty (element, clash.property) || song.getValues() == nullptr)
            return AreaChoice::notTheArea;

        if (! juce::isPositiveAndBelow (value, 3))
            return AreaChoice::refused;

        juce::ValueTree holder (element.getType());
        holder.setProperty (schema::trackID, element[schema::trackID], nullptr);
        auto switches = getTrackSwitches (song);
        writeSwitchesInto (holder, switches);

        const juce::var* values[] = { &clash.base, &clash.ours, &clash.theirs };

        if (values[value]->isVoid())
            holder.removeProperty (clash.property, nullptr);
        else
            holder.setProperty (clash.property, *values[value], nullptr);

        readHeldSwitchesOf (holder, switches);
        setTrackSwitches (song, std::move (switches), true);
        return AreaChoice::chosen;
    }

    // A whole track or the master ("TRACKS:TRACKS", "TRACK:12"; "MASTER:MASTER"): a path that ends at one, with no property.
    // A merge of two songs Phazite holds never reports one (it keeps a track taken away on one side and changed on the other,
    // and merges the rest property by property: SongMerge.cpp); a commit's song may come from anyone.
    if (clash.path.isEmpty())
        return AreaChoice::notTheArea;

    if (const auto last = clash.path[clash.path.size() - 1];
        ! last.startsWith (schema::TRACK.toString() + ":") && ! last.startsWith (schema::MASTER.toString() + ":"))
        return AreaChoice::notTheArea;

    // Nothing written for a path whose place the song hasn't.
    auto parentPath = clash.path;
    parentPath.remove (parentPath.size() - 1);
    auto parent = findByPath (song.getState(), parentPath);

    if (! parent.isValid() || ! juce::isPositiveAndBelow (value, 3))
        return AreaChoice::refused;

    // The song's, in its place, with what it holds in the song's areas (eraseTrackHoldings()); then the side chosen's in that
    // place, where it has one: its element without its tracks' clips and sends and its owners' modulators and macros, which
    // each area's code then gives the song as the keeper took them in (Clips.h: takeClipsOf(); Modulation.h:
    // takeModulatorsOf(); Mixer.h: takeSendsOf(); its devices, containers and chains: Mixer.h, takeDevicesOf(); what they hold
    // of the store: DeviceStore.h, setStoreEntries()), so that nothing is left to the keeper.
    auto* undo = &song.getUndoManager();
    const auto existing = findByPath (song.getState(), clash.path);
    const juce::ValueTree* elements[] = { &clash.baseElement, &clash.oursElement, &clash.theirsElement };
    const auto& chosen = *elements[value];
    const auto place = existing.isValid() ? parent.indexOf (existing) : -1;

    // The tracks the song holds in the element it replaces, by their IDs: what comes back under one of them takes its switches
    // in the step (below).
    std::set<std::uint64_t> heldBefore;

    if (existing.isValid())
    {
        std::vector<juce::ValueTree> replaced;
        collectTrackElements (existing, replaced);

        for (const auto& element : replaced)
            heldBefore.insert (readID (element[schema::trackID]));
    }

    if (existing.isValid())
    {
        eraseTrackHoldings (song, { getTrackID (existing) });
        parent.removeChild (existing, undo);
    }

    // A clip on another track left the only one of its alias group (the song's track held the others, which the side's
    // holds in no group, or not at all) leaves the group, as deleteTracks() unlinks it.
    if (! chosen.isValid())
    {
        unlinkLoneClips (song);
        return AreaChoice::chosen;
    }

    // What the keeper's parts would take out of the element as it comes (its tracks' CLIP and SEND elements, its owners'
    // MODULATOR and MACRO elements, what its devices and chains hold of the store, its channels' DEVICE and CONTAINER
    // elements), taken out of a copy of it first; then what they would read of it, read from the side's element and given to
    // the song once the copy is in it: the clips', the devices' (before modulation's: a container's macros need their
    // container), modulation's, the sends', the store's.
    auto copy = chosen.createCopy();
    std::vector<juce::ValueTree> tracks, owners, items;
    collectTrackElements (copy, tracks);
    collectModulationOwners (copy, owners);
    collectDeviceItems (copy, items);

    for (const auto& track : tracks)
        removeChildrenOf (track, schema::CLIP, schema::SEND);

    for (const auto& owner : owners)
        removeChildrenOf (owner, schema::MODULATOR, schema::MACRO);

    for (const auto& item : items)
        takeStoreOutOf (item, nullptr);

    for (const auto& track : tracks)
        removeChildrenOf (track, schema::DEVICE, schema::CONTAINER);

    if (copy.hasType (schema::MASTER))
        removeChildrenOf (copy, schema::DEVICE, schema::CONTAINER);

    // Its tracks' switches and view (the master's mode and row), read from the side's elements as the song holds them and
    // given through their areas before the copy comes in, taken out of the copy (TrackSwitchesValue.h, TracksOutside.h), as a
    // version's track that comes back gets them (model/versions/ApplySong.cpp: makeTrackLeavesLike()): the switches in the
    // step, the song's entries of their IDs replaced, but those of a track new to the song (neither in the element replaced
    // nor of an ID the switches hold any entry of: holdsSwitchesOf()), which are written outside the history, as a maker gives
    // a new item's (copyTrackLeaves()); the view of each where the song keeps none of its ID (a row it keeps, of a track it
    // held, stays), the master's where its row holds nothing set.
    if (auto* values = song.getValues())
    {
        const auto held = values->trackSwitches.get();
        auto newSwitches = held;
        auto outside = values->tracksOutside.get();
        std::vector<juce::ValueTree> chosenTracks, inStep;
        collectTrackElements (chosen, chosenTracks);

        if (chosen.hasType (schema::MASTER))
            chosenTracks.push_back (chosen);

        for (const auto& element : chosenTracks)
        {
            if (element.hasType (schema::MASTER))
            {
                inStep.push_back (element);

                if (outside.master == TrackView())
                    outside = withMasterView (outside, readHeldViewOf (element));

                continue;
            }

            const auto id = readID (element[schema::trackID]);

            if (heldBefore.count (id) == 0 && ! holdsSwitchesOf (held, id))
                readHeldSwitchesOf (element, newSwitches);
            else
                inStep.push_back (element);

            if (id > 0 && id < schema::maxID && outside.tracks.find (id) == nullptr)
                outside = withTrackView (outside, id, readHeldViewOf (element));
        }

        values->setTracksOutside (std::move (outside));
        values->setSwitchesOutsideHistory (song, std::move (newSwitches));
        auto switches = values->trackSwitches.get();

        for (const auto& element : inStep)
            readHeldSwitchesOf (element, switches);

        values->setSwitches (song, std::move (switches));
    }

    for (const auto& track : tracks)
    {
        takeSwitchesOutOf (track, nullptr);
        takeViewOutOf (track, nullptr);
    }

    if (copy.hasType (schema::MASTER))
    {
        takeSwitchesOutOf (copy, nullptr);
        takeViewOutOf (copy, nullptr);
    }

    parent.addChild (copy, place, undo);
    tracks.clear();
    owners.clear();
    items.clear();
    collectTrackElements (chosen, tracks);
    collectModulationOwners (chosen, owners);
    collectDeviceItems (chosen, items);

    for (const auto& track : tracks)
        takeClipsOf (song, track);

    for (const auto& track : tracks)
        takeDevicesOf (song, track);

    if (chosen.hasType (schema::MASTER))
        takeDevicesOf (song, chosen);

    for (const auto& owner : owners)
        takeModulatorsOf (song, owner);

    for (const auto& track : tracks)
        takeSendsOf (song, track);

    // Each plug-in's and chain's entries made the side's, as the versions' devices pass makes a version's (ApplySong.cpp:
    // makeDeviceStoreLike()): those the store kept of an item of the same ID (the song's track's, taken out above) replaced
    // (model/plugins/internal/DeviceStore.h: setStoreEntries()); a device's window's place given where the side has one, as
    // view state outside the history.
    for (const auto& item : items)
    {
        const auto id = readDeviceElementID (item);
        DeviceStore entries;
        readHeldStoreOf (item, entries);
        setStoreEntries (song, id, entries);

        if (const auto window = item.hasType (schema::DEVICE) ? readWindowPlaceOf (item) : std::nullopt)
            setWindowPlace (song, id, *window);
    }

    unlinkLoneClips (song);
    return AreaChoice::chosen;
}

//==============================================================================
void addTrackCommands (CommandRegistry& registry, Selection& selection)
{
    const auto undoTarget = undoTargetOf (selection);
    const auto hasSong = [&selection] { return selection.getSong() != nullptr; };

    registry.add ({ .id = "track.add",
                    .name = "Add Track",
                    .category = "Track",
                    .description = "Adds an empty track below the selected one, or at the bottom.",
                    .synonyms = { "new track", "insert track", "create" },
                    .perform = [&selection] { insertTrackAfterSelection (selection); },
                    .isEnabled = hasSong,
                    .undoTarget = undoTarget });

    // Folders.
    registry.add ({ .id = "track.addFolder",
                    .name = "Add Folder",
                    .category = "Track",
                    .description = "Adds an empty folder below the selected track, or at the bottom.",
                    .synonyms = { "new folder", "group", "bus" },
                    .perform = [&selection] { insertFolderAfterSelection (selection); },
                    .isEnabled = hasSong,
                    .undoTarget = undoTarget });

    registry.add ({ .id = "track.groupIntoFolder",
                    .name = "Group into Folder",
                    .category = "Track",
                    .description = "Puts the selected tracks into a new folder. A folder becomes a bus when it holds an effect.",
                    .synonyms = { "group", "folder", "bus", "submix" },
                    .perform = withSong (selection, [&selection] (Song& song)
                                         { groupIntoFolder (song, getSelectedOutermostTracks (selection), &selection); }),
                    .isEnabled = [&selection] { return hasSelectedTracks (selection); },
                    .undoTarget = undoTarget });

    registry.add ({ .id = "track.ungroup",
                    .name = "Ungroup Folder",
                    .category = "Track",
                    .description = "Takes the tracks out of the selected folders, and deletes the folders.",
                    .synonyms = { "remove folder", "ungroup", "unfolder" },
                    .perform = withSong (selection, [&selection] (Song& song) { ungroupFolders (song, getSelectedFolders (selection)); }),
                    .isEnabled = [&selection] { return hasSelectedFolders (selection); },
                    .undoTarget = undoTarget });

    // Collapsing and pinning change the view, so they aren't undo steps.
    registry.add (
        { .id = "track.collapse",
          .name = "Collapse Track",
          .category = "Track",
          .description = "Makes the selected tracks thin rows that show only their names, or full height again. A "
                         "collapsed folder hides its tracks.",
          .synonyms = { "fold", "unfold", "expand", "hide", "minimize", "folder" },
          .perform = withSong (selection, [&selection] (Song& song) { toggleTracksCollapsed (song, getSelectedTracks (selection)); }),
          .isEnabled = [&selection] { return hasSelectedTracks (selection); },
          .isTicked = ifSong (selection,
                              [&selection] (const Song& song) { return areAllTracksCollapsed (song, getSelectedTracks (selection)); }) });

    registry.add ({ .id = "track.collapseAll",
                    .name = "Collapse All",
                    .category = "Track",
                    .description = "Makes every track, folder and automation lane a thin row, or, when they all are, shows them "
                                   "all at full height again.",
                    .synonyms = { "fold all", "unfold all", "expand all", "collapse everything", "minimize all" },
                    .perform = withSong (selection, [] (Song& song) { toggleAllRowsCollapsed (song); }),
                    .isEnabled = hasSong,
                    .currentName = [&selection]
                    {
                        auto* song = selection.getSong();
                        return song == nullptr || isAnyRowExpanded (*song) ? juce::String ("Collapse All") : juce::String ("Expand All");
                    } });

    registry.add (
        { .id = "track.pin",
          .name = "Pin Track",
          .category = "Track",
          .description = "Keeps the selected tracks in view at the top of the playlist while the others scroll.",
          .synonyms = { "stick", "keep visible", "unpin" },
          .perform = withSong (selection, [&selection] (Song& song) { toggleTracksPinned (song, getSelectedTracks (selection)); }),
          .isEnabled = [&selection] { return hasSelectedTracks (selection); },
          .isTicked =
              ifSong (selection, [&selection] (const Song& song) { return areAllTracksPinned (song, getSelectedTracks (selection)); }) });

    registry.add ({ .id = "track.selectPrevious",
                    .name = "Select Track Above",
                    .category = "Track",
                    .description = "Selects the track above the selected one.",
                    .synonyms = { "previous track", "up" },
                    .perform = [&selection] { selectNeighbourTrack (selection, -1); },
                    .isEnabled = hasSong,
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "track.selectNext",
                    .name = "Select Track Below",
                    .category = "Track",
                    .description = "Selects the track below the selected one.",
                    .synonyms = { "next track", "down" },
                    .perform = [&selection] { selectNeighbourTrack (selection, 1); },
                    .isEnabled = hasSong,
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "track.moveUp",
                    .name = "Move Track Up",
                    .category = "Track",
                    .description = "Moves the selected tracks up one place.",
                    .synonyms = { "reorder", "raise" },
                    .perform = [&selection] { moveSelectedTracks (selection, -1); },
                    .isEnabled = [&selection] { return hasSelectedTracks (selection); },
                    .undoTarget = undoTarget,
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "track.moveDown",
                    .name = "Move Track Down",
                    .category = "Track",
                    .description = "Moves the selected tracks down one place.",
                    .synonyms = { "reorder", "lower" },
                    .perform = [&selection] { moveSelectedTracks (selection, 1); },
                    .isEnabled = [&selection] { return hasSelectedTracks (selection); },
                    .undoTarget = undoTarget,
                    .repeatsWhileHeld = true });

    // Mute and solo are kept out of the undo history, as many DAWs do.
    registry.add ({ .id = "track.mute",
                    .name = "Mute Track",
                    .category = "Track",
                    .description = "Silences the selected tracks, or unmutes them.",
                    .synonyms = { "silence", "unmute" },
                    .perform = withSong (selection, [&selection] (Song& song) { toggleTracksMuted (song, getSelectedTracks (selection)); }),
                    .isEnabled = [&selection] { return hasSelectedTracks (selection); },
                    .isTicked = ifSong (selection, [&selection] (const Song& song)
                                        { return areAllTracksMuted (song, getSelectedTracks (selection)); }) });

    registry.add (
        { .id = "track.solo",
          .name = "Solo Track",
          .category = "Track",
          .description = "Plays only the selected tracks and any others that are soloed, or ends their solo.",
          .synonyms = { "isolate", "unsolo" },
          .perform = withSong (selection, [&selection] (Song& song) { toggleTracksSoloed (song, getSelectedTracks (selection)); }),
          .isEnabled = [&selection] { return hasSelectedTracks (selection); },
          .isTicked =
              ifSong (selection, [&selection] (const Song& song) { return areAllTracksSoloed (song, getSelectedTracks (selection)); }) });
}
} // namespace phazite
