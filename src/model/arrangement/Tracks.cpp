// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Tracks.h"
#include "internal/Clips.h"
#include "model/automation/internal/Automation.h"
#include "model/base/Toggles.h"
#include "model/base/internal/DeviceStoreValue.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/TrackSwitchesValue.h"
#include "model/base/internal/TracksOutside.h"
#include "model/base/internal/TracksValue.h"
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
#include <atomic>
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

    // A track moved to a place in a folder's list (setTrackPlace(): its place alone), and where it came to another folder,
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
        const auto& held = getTracksValue (song);
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
    // the top list); into it, after its last track, or else before the first track of its list from the target on that
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
        const auto& order = getTrackOrder (song).order;
        const auto& colours = getTrackColours();
        size_t next = 0;

        if (const auto above = after.isValid() ? after : (order.empty() ? TrackID() : order.back()); above.isValid())
        {
            const auto row = (int) (std::find (order.begin(), order.end(), above) - order.begin());
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
        const auto& tracks = getTracksValue (song);
        const auto* value = tracks.tracks.find (track.getRaw());
        const auto* list = value != nullptr ? tracks.findList (value->parent) : nullptr;

        if (const auto i = findListIndex (tracks, track) + direction; list != nullptr && juce::isPositiveAndBelow (i, (int) list->size()))
            return (*list)[(size_t) i];

        return {};
    }

    // Whether a track's switch is on (a look in the switches' map by its ID), or a folder's it is in (up its folders).
    bool isOnOrInFolderOn (const SongTracks& tracks, const immer::map<std::uint64_t, bool>& switches, TrackID track)
    {
        for (const auto* value = tracks.tracks.find (track.getRaw()); value != nullptr; value = tracks.tracks.find (value->parent.getRaw()))
            if (switches.find (value->id.getRaw()) != nullptr)
                return true;

        return false;
    }

    // The track whose drum sampler a pad's track plays a pad of (model/devices/internal/DrumKit.h), whose mute and solo cover
    // it as a folder's cover its tracks; none for any other track.
    TrackID findPadSamplerTrack (const Song& song, const SongTracks& tracks, TrackID track)
    {
        const auto* value = tracks.tracks.find (track.getRaw());

        if (value == nullptr || ! value->pad.has_value())
            return {};

        const auto sampler = getDeviceChannel (song, value->padDevice);
        return sampler != track && tracks.tracks.find (sampler.getRaw()) != nullptr ? sampler : TrackID();
    }

    // A channel's value: the master's by its flag, else its track's; nullptr for none. A pointer into the value given.
    const TrackValue* findChannel (const SongTracks& tracks, ChannelRef channel)
    {
        if (channel.onMaster)
            return &tracks.master;

        return tracks.find (channel.track); // the master by its ID too
    }

    // A channel's fader and VCA: the area's by a look (findTrack()).
    struct ChannelDevices
    {
        DeviceID fader {}, vca {};
    };

    ChannelDevices findChannelDevices (const Song& song, ChannelRef channel)
    {
        const auto* value = findChannel (getTracksValue (song), channel);
        return value != nullptr ? ChannelDevices { .fader = value->fader, .vca = value->vca } : ChannelDevices {};
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
        const auto& tracks = getTracksValue (song);

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
TrackLookup::TrackLookup (const TrackValue* inArea) noexcept
    : found (inArea)
{
}

const TrackValue* TrackLookup::operator->() const& noexcept
{
    jassert (found != nullptr); // a lookup that found none holds no track
    return found;
}

const TrackValue& TrackLookup::operator*() const& noexcept
{
    jassert (found != nullptr); // a lookup that found none holds no track
    return *found;
}

namespace
{
    std::atomic<std::uint64_t> trackLooks { 0 }; // countTrackLooks()
} // namespace

std::uint64_t countTrackLooks() noexcept
{
    return trackLooks.load (std::memory_order_relaxed);
}

TrackLookup findTrack (const Song& song, TrackID id)
{
    trackLooks.fetch_add (1, std::memory_order_relaxed);
    return TrackLookup (song.getValues()->tracks.get().find (id));
}

bool hasTrack (const Song& song, TrackID id)
{
    return (bool) findTrack (song, id);
}

const SongTracks& getTracksValue (const Song& song)
{
    return song.getValues()->tracks.get();
}

const TrackOrder& getTrackOrder (const Song& song)
{
    return song.getValues()->getTrackOrder();
}

DeviceID getChannelFader (const Song& song, ChannelRef channel)
{
    return findChannelDevices (song, channel).fader;
}

DeviceID getChannelVCA (const Song& song, ChannelRef channel)
{
    return findChannelDevices (song, channel).vca;
}

bool isFaderOf (const Song& song, ChannelRef channel, DeviceID device)
{
    const auto devices = findChannelDevices (song, channel);
    return device.isValid() && (devices.fader == device || devices.vca == device);
}

TrackOrder::Place getTrackPlace (const Song& song, TrackID id)
{
    if (! hasTrack (song, id) || isMaster (song, id))
        return {};

    return getTrackOrder (song).getPlace (id);
}

std::set<std::uint64_t> getAutomatableIDs (const Song& song, TrackID id)
{
    std::set<std::uint64_t> ids;
    const auto track = findTrack (song, id);

    if (! track)
        return ids;

    for (const auto device : { track->fader, track->vca })
        if (device.isValid())
            ids.insert (device.getRaw());

    const auto onMaster = track->isMaster();

    for (const auto& device : getDeviceOutline (song, id))
        ids.insert (device.id.getRaw());

    for (const auto send : getSends (song, id))
        ids.insert (send.getRaw());

    const auto modulation = getModulationValue (song);

    for (const auto& entry : modulation.modulators)
        if (const auto& place = entry.second.place; onMaster ? place.onMaster : ! place.onMaster && place.track == id)
            ids.insert (entry.first);

    return ids;
}

std::optional<ChannelRef> findChannelOnPath (const Song& song, const juce::StringArray& path)
{
    const auto isStepOf = [] (const juce::String& step, const juce::Identifier& type) { return step.startsWith (type.toString() + ":"); };

    if (path.size() == 1 && isStepOf (path[0], schema::MASTER))
        return ChannelRef { .onMaster = true };

    if (path.size() < 2 || ! isStepOf (path[0], schema::TRACKS))
        return std::nullopt;

    const auto& tracks = getTracksValue (song);
    TrackID folder;

    for (int i = 1; i < path.size(); ++i)
    {
        if (! isStepOf (path[i], schema::TRACK))
            return std::nullopt;

        const auto id = TrackID::fromRaw (readID (path[i].fromFirstOccurrenceOf (":", false, false)));

        if (const auto* value = tracks.tracks.find (id.getRaw()); value == nullptr || value->parent != folder)
            return std::nullopt;

        folder = id;
    }

    return ChannelRef { .track = folder };
}

//==============================================================================
TrackID getMasterID (const Song& song)
{
    return song.getValues()->tracks.get().master.id;
}

std::vector<TrackID> getTrackIDs (const Song& song)
{
    return getTrackOrder (song).order;
}

std::optional<TrackValue::Kind> getTrackKind (const Song& song, TrackID id)
{
    if (const auto track = findTrack (song, id))
        return track->kind;

    return std::nullopt;
}

std::vector<TrackID> getTrackIDsOfKind (const Song& song, TrackValue::Kind kind)
{
    const auto& tracks = getTracksValue (song);
    std::vector<TrackID> ids;

    for (const auto id : getTrackOrder (song).order)
        if (const auto* value = tracks.tracks.find (id.getRaw()); value != nullptr && value->kind == kind)
            ids.push_back (id);

    return ids;
}

juce::String getTrackInput (const Song& song, TrackID id)
{
    const auto track = findTrack (song, id);
    return track ? track->input : juce::String();
}

TrackValue::Monitoring getTrackMonitoring (const Song& song, TrackID id)
{
    const auto track = findTrack (song, id);
    return track ? track->monitoring : TrackValue::Monitoring::whileArmed;
}

bool isFolder (const Song& song, TrackID id)
{
    const auto track = findTrack (song, id);
    return track && track->isFolder();
}

bool isReturnTrack (const Song& song, TrackID id)
{
    const auto track = findTrack (song, id);
    return track && track->isReturn();
}

bool isMaster (const Song& song, TrackID id)
{
    return id.isValid() && id == getMasterID (song);
}

juce::String getTrackName (const Song& song, TrackID id)
{
    const auto track = findTrack (song, id);
    return track ? getTrackName (song, *track) : juce::String();
}

juce::String getTrackName (const Song& song, const TrackValue& track)
{
    if (track.isMaster())
        return "Master";

    if (const auto name = track.getOwnName(); name.isNotEmpty() || track.isFolder())
        return name;

    // Its number among the tracks that aren't folders, as the song's order keeps it: every track the song holds has one.
    const auto number = getTrackOrder (song).getPlace (track.id).number;
    jassert (number > 0);
    return number > 0 ? "Track " + juce::String (number) : juce::String ("Track");
}

TrackID getParentFolder (const Song& song, TrackID id)
{
    if (const auto* value = getTracksValue (song).tracks.find (id.getRaw()))
        return value->parent;

    return {};
}

std::vector<TrackID> getTracksInFolder (const Song& song, TrackID folder)
{
    const auto& tracks = getTracksValue (song);
    std::vector<TrackID> ids;

    const std::function<void (TrackID)> add = [&tracks, &ids, &add] (TrackID owner)
    {
        if (const auto* list = tracks.findList (owner))
        {
            for (const auto id : *list)
            {
                ids.push_back (id);
                add (id);
            }
        }
    };

    if (tracks.tracks.find (folder.getRaw()) != nullptr)
        add (folder);

    return ids;
}

bool isTrackMuted (const Song& song, TrackID id, bool orInMutedFolder)
{
    const auto& tracks = getTracksValue (song);

    if (tracks.tracks.find (id.getRaw()) == nullptr)
        return false;

    const auto switches = getTrackSwitches (song);

    if (! orInMutedFolder)
        return switches.mutes.find (id.getRaw()) != nullptr;

    // A pad's track is muted with its sampler's track, as a track is with its folder.
    return isOnOrInFolderOn (tracks, switches.mutes, id)
           || isOnOrInFolderOn (tracks, switches.mutes, findPadSamplerTrack (song, tracks, id));
}

bool isTrackSoloed (const Song& song, TrackID id)
{
    return getTrackSwitches (song).solos.find (id.getRaw()) != nullptr && getTracksValue (song).tracks.find (id.getRaw()) != nullptr;
}

bool areAnyTracksSoloed (const Song& song)
{
    // Each solo the switches hold, of a track the song holds (one a step took away keeps its entry, for an Undo).
    const auto solos = getTrackSwitches (song).solos;
    const auto& tracks = getTracksValue (song);
    return std::any_of (solos.begin(), solos.end(), [&tracks] (const auto& entry) { return tracks.tracks.find (entry.first) != nullptr; });
}

bool isTrackFrozen (const Song& song, TrackID id)
{
    return getTrackSwitches (song).frozen.find (id.getRaw()) != nullptr && getTracksValue (song).tracks.find (id.getRaw()) != nullptr;
}

std::optional<TrackPad> getTrackPad (const Song& song, TrackID id)
{
    if (const auto* value = getTracksValue (song).tracks.find (id.getRaw()); value != nullptr && value->pad.has_value())
        return TrackPad { .track = id, .sampler = value->padDevice, .pad = *value->pad };

    return std::nullopt;
}

std::vector<TrackPad> getPadTracks (const Song& song)
{
    const auto& tracks = getTracksValue (song);
    std::vector<TrackPad> pads;

    for (const auto id : getTrackOrder (song).order)
        if (const auto* value = tracks.tracks.find (id.getRaw()); value != nullptr && value->pad.has_value())
            pads.push_back ({ .track = id, .sampler = value->padDevice, .pad = *value->pad });

    return pads;
}

void setTrackMuted (Song& song, TrackID id, bool muted, juce::UndoManager* undo)
{
    // Off is no entry at all, as it is at first: unmuted again, the track is as it was, which a version's changes then don't
    // count (docs/design/versions.md). One entry set, the switches' setter asked only where it differs.
    if (getTracksValue (song).tracks.find (id.getRaw()) != nullptr)
        setTrackSwitches (song, withMute (getTrackSwitches (song), id.getRaw(), muted), undo != nullptr);
}

void setTrackSoloed (Song& song, TrackID id, bool soloed)
{
    if (getTracksValue (song).tracks.find (id.getRaw()) != nullptr)
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
    const auto track = findTrack (song, id);
    return getTrackColour (track ? track->colour : std::nullopt, row);
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
    const auto* values = song.getValues();

    if (isMaster (song, id))
        return values->tracksOutside.get().master;

    const auto* view =
        values->tracks.get().tracks.find (id.getRaw()) != nullptr ? values->tracksOutside.get().tracks.find (id.getRaw()) : nullptr;
    return view != nullptr ? *view : TrackView();
}

TrackSwitches getTrackSwitches (const Song& song)
{
    return song.getValues()->trackSwitches.get();
}

TracksOutside getTracksOutside (const Song& song)
{
    return song.getValues()->tracksOutside.get();
}

std::optional<std::uint64_t> findSwitchKey (const Song& song, TrackID id)
{
    const auto track = findTrack (song, id);

    if (! track)
        return std::nullopt;

    return track->isMaster() ? (std::uint64_t) 0 : id.getRaw();
}

void setTrackSwitches (Song& song, TrackSwitches next, bool inHistory)
{
    auto* values = song.getValues();

    if (inHistory)
        values->setSwitches (song, std::move (next));
    else
        values->setSwitchesOutsideHistory (song, std::move (next));
}

void setTrackView (Song& song, TrackID id, TrackView view)
{
    auto* values = song.getValues();
    const auto key = findSwitchKey (song, id);

    if (! key.has_value())
        return;

    const auto& outside = values->tracksOutside.get();
    values->setTracksOutside (*key == 0 ? withMasterView (outside, std::move (view)) : withTrackView (outside, *key, std::move (view)));
}

void setTrackFrozen (Song& song, TrackID id, bool frozen, juce::UndoManager* undo)
{
    if (getTracksValue (song).tracks.find (id.getRaw()) != nullptr)
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
    const auto& tracks = getTracksValue (song);
    const auto* value = tracks.tracks.find (id.getRaw());

    for (auto folder = value != nullptr ? value->parent : TrackID(); folder.isValid();)
    {
        if (getTrackView (song, folder).collapsed.value_or (false))
            return true;

        const auto* parent = tracks.tracks.find (folder.getRaw());
        folder = parent != nullptr ? parent->parent : TrackID();
    }

    return false;
}

void toggleSoloAlone (Song& song, TrackID id)
{
    const auto& tracks = getTracksValue (song);

    if (tracks.tracks.find (id.getRaw()) == nullptr)
        return;

    // Every other track's solo off (of the tracks the song holds: a track a step took away keeps its entry), and its own on,
    // or off where it was soloed alone: one set of the whole next value, one call to the listeners.
    auto next = getTrackSwitches (song);
    auto alone = isTrackSoloed (song, id);
    auto solos = next.solos;

    for (const auto& entry : next.solos)
    {
        if (entry.first == id.getRaw() || tracks.tracks.find (entry.first) == nullptr)
            continue;

        alone = false;
        solos = solos.erase (entry.first);
    }

    next.solos = alone ? solos.erase (id.getRaw()) : solos.set (id.getRaw(), true);
    setTrackSwitches (song, std::move (next), false);
}

bool isSilencedBySolo (const Song& song, TrackID id)
{
    const auto& tracks = getTracksValue (song);
    const auto* value = tracks.tracks.find (id.getRaw());

    if (value == nullptr)
        return false;

    const auto solos = getTrackSwitches (song).solos;
    return areAnyTracksSoloed (song) && ! isOnOrInFolderOn (tracks, solos, id)
           && ! isOnOrInFolderOn (tracks, solos, findPadSamplerTrack (song, tracks, id)) && ! value->isReturn()
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
        const auto switchesBefore = values->trackSwitches.get();
        const auto outsideBefore = values->tracksOutside.get();
        copyTrackLeaves (song, made.originalsAndCopies, copied);

        if (! setNewTracks (song, made.values, findListIndex (getHeldTracks (song), track) + 1))
        {
            values->setTracksOutside (outsideBefore);
            values->setSwitchesOutsideHistory (song, switchesBefore);

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
    if (tracks.empty())
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

        if (getTracksValue (song).tracks.find (id.getRaw()) == nullptr)
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
    // The tracks of a TRACK element, itself first and then those in it, depth first: those of a side's track a merge's choice
    // brings in whole, whose holdings each area's code takes in (giveHoldingsOf(), below).
    void collectTrackElements (const juce::ValueTree& element, std::vector<juce::ValueTree>& tracks)
    {
        if (! element.hasType (schema::TRACK))
            return;

        tracks.push_back (element);

        for (const auto& child : element)
            collectTrackElements (child, tracks);
    }

    // The owners of modulators and macros in a channel's element, as modulation's reader of a whole channel takes them
    // (model/base/ModulationArea.cpp: takeChannelModulatorsIn()): the channel, then each of its containers and, a folder's,
    // each of its tracks with theirs, in its elements' order.
    void collectModulationOwners (const juce::ValueTree& owner, std::vector<juce::ValueTree>& owners)
    {
        owners.push_back (owner);

        if (owner.hasType (schema::CONTAINER))
            return;

        for (const auto& child : owner)
            if (child.hasType (schema::CONTAINER)
                || (child.hasType (schema::TRACK) && owner.hasType (schema::TRACK)
                    && owner[schema::trackKind].toString() == schema::trackIsFolder))
                collectModulationOwners (child, owners);
    }

    // Each DEVICE and CHAIN in an element, through what can hold one (a track, a folder's tracks, the master, a container, a
    // chain), in the tree's order.
    void collectDeviceItems (const juce::ValueTree& element, std::vector<juce::ValueTree>& items)
    {
        if (element.hasType (schema::DEVICE) || element.hasType (schema::CHAIN))
            items.push_back (element);

        if (element.hasType (schema::TRACK) || element.hasType (schema::MASTER) || element.hasType (schema::CONTAINER)
            || element.hasType (schema::CHAIN))
            for (const auto& child : element)
                collectDeviceItems (child, items);
    }

    // The track a merge's clash's path names, by the steps that name it ("TRACKS:TRACKS", then a "TRACK:12" for each folder
    // it is in and for itself; or "MASTER:MASTER"), read from the song's value of its tracks (getHeldTracks(): no element
    // read): whether the path goes past it, into what is in it; whether the song holds the folders the path names it in, each
    // in the one before (placeHeld); and the song's own, where the song holds it there (held).
    struct ClashTrack
    {
        bool master = false;
        TrackID id {};
        TrackID folder {}; // the folder the path names it in, none for the track list
        bool deeper = false;
        bool placeHeld = false;
        std::optional<TrackValue> held {};
    };

    std::optional<ClashTrack> findClashTrack (const Song& song, const juce::StringArray& path)
    {
        const auto isStepOf = [] (const juce::String& step, const juce::Identifier& type)
        { return step.startsWith (type.toString() + ":"); };

        if (path.isEmpty())
            return std::nullopt;

        const auto tracks = getHeldTracks (song);
        ClashTrack found;

        if (isStepOf (path[0], schema::MASTER))
        {
            found.master = true;
            found.id = tracks.master.id;
            found.deeper = path.size() > 1;
            found.placeHeld = true;
            found.held = tracks.master;
            return found;
        }

        if (! isStepOf (path[0], schema::TRACKS))
            return std::nullopt;

        found.placeHeld = true;
        auto heldThere = true;

        for (int i = 1; i < path.size() && ! found.deeper; ++i)
        {
            if (! isStepOf (path[i], schema::TRACK))
            {
                found.deeper = true;
                continue;
            }

            found.placeHeld = found.placeHeld && heldThere;
            found.folder = found.id;
            found.id = TrackID::fromRaw ((std::uint64_t) path[i].fromFirstOccurrenceOf (":", false, false).getLargeIntValue());
            const auto* held = tracks.tracks.find (found.id.getRaw());
            heldThere = held != nullptr && held->parent == found.folder;
        }

        if (found.id.isValid() && heldThere)
            found.held = *tracks.tracks.find (found.id.getRaw());

        return found;
    }

    // Whether the song can hold a track's (or the master's) fields chosen in a merge in place of what it holds of it, as the
    // tracks' setter can't tell on its own: no kind that holds no clips for a track that holds some (Clips.h: canTrackHold()),
    // and no ID (the master's own, a fader's, a VCA's) that the song holds as another track's, fader or VCA, or as a device's.
    bool canHoldChosen (const Song& song, const TrackValue& held, const TrackValue& next)
    {
        if (! next.isMaster() && ! next.holdsClips() && ! getClips (song, next.id).empty())
            return false;

        std::set<std::uint64_t> others;
        forEachIDHeld (getHeldTracks (song), [&others] (std::uint64_t id) { others.insert (id); });

        for (const auto own : { held.id.getRaw(), held.fader.getRaw(), held.vca.getRaw() })
            others.erase (own);

        for (const auto& [now, was] :
             { std::pair (next.id.getRaw(), held.id.getRaw()), std::pair (next.fader.getRaw(), held.fader.getRaw()),
               std::pair (next.vca.getRaw(), held.vca.getRaw()) })
            if (now != 0 && now != was && (others.count (now) > 0 || findDevice (song, DeviceID::fromRaw (now)) != nullptr))
                return false;

        return true;
    }

    // A property of a track's or the master's own chosen (ClashTrack: the song holds it where the path names it): a switch (a
    // track's mute, solo or frozen switch, its or the master's automation mode), its entry in the switches given the value
    // chosen, or taken away where that side had none, in the choice's step, as the conversion reads it
    // (TrackSwitchesValue.h: readHeldSwitchesOf(), of the element with the song's other entries of it and the value chosen);
    // else a field of the track's value (TracksValue.h: setTrackProperty(), as readHeldTrack() reads one; its default where
    // that side held none), through the tracks' setter (setTrack()). Refused: a track the song hasn't there; a property the
    // value doesn't cover (one Phazite doesn't know: the view counts nothing, and a merge reports no clash of it); a track's
    // ID, which is its key; and fields the song couldn't hold (canHoldChosen(); setTrack()'s refusals: a folder made a track
    // while it holds tracks).
    AreaChoice chooseTrackProperty (Song& song, const ClashTrack& track, const MergeClash& clash, const juce::var& chosen)
    {
        if (! track.held.has_value())
            return AreaChoice::refused;

        juce::ValueTree holder (track.master ? schema::MASTER : schema::TRACK);
        holder.setProperty (schema::trackID, (juce::int64) track.id.getRaw(), nullptr);

        if (isSwitchProperty (holder, clash.property))
        {
            auto switches = getTrackSwitches (song);
            writeSwitchesInto (holder, switches);

            if (chosen.isVoid())
                holder.removeProperty (clash.property, nullptr);
            else
                holder.setProperty (clash.property, chosen, nullptr);

            readHeldSwitchesOf (holder, switches);
            setTrackSwitches (song, std::move (switches), true);
            return AreaChoice::chosen;
        }

        auto next = *track.held;

        if ((clash.property == schema::trackID && ! track.master) || ! setTrackProperty (next, clash.property, chosen))
            return AreaChoice::refused;

        if (next == *track.held)
            return AreaChoice::chosen;

        if (! canHoldChosen (song, *track.held, next) || ! setTrack (song, std::move (next)))
            return AreaChoice::refused;

        return AreaChoice::chosen;
    }

    // The switches and the view of the tracks of a side's element chosen whole (the master's mode and row), read from its
    // elements as the song holds them and given through their areas before its values come in (TrackSwitchesValue.h,
    // TracksOutside.h), as a version's track that comes back gets them (model/versions/ApplySong.cpp: makeTrackLeavesLike()):
    // the switches in the step, the song's entries of their IDs replaced, but those of a track new to the song (neither one of
    // the tracks replaced, heldBefore, nor of an ID the switches hold any entry of: holdsSwitchesOf()), which are written
    // outside the history, as a maker gives a new item's (copyTrackLeaves()); the view of each where the song keeps none of
    // its ID (a row it keeps, of a track it held, stays), the master's where its row holds nothing set.
    void giveLeavesOf (Song& song, const juce::ValueTree& chosen, const std::set<std::uint64_t>& heldBefore)
    {
        auto* values = song.getValues();

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

    // What a side's element chosen whole holds in the song's areas, read from it and given to the song once its tracks (or the
    // master) are in it, each through its area's code: its tracks' clips with
    // their notes (Clips.h: takeClipsOf()), its channels' devices, containers and chains (Mixer.h: takeDevicesOf(): before
    // modulation's, as a container's macros need their container), its owners' modulators and macros (Modulation.h:
    // takeModulatorsOf()), its tracks' sends (Mixer.h: takeSendsOf()), and each plug-in's and chain's entries in the devices'
    // store made the side's, as the versions' devices pass makes a version's (ApplySong.cpp: makeDeviceStoreLike()): those the
    // store kept of an item of the same ID (the song's, taken out before) replaced (model/plugins/internal/DeviceStore.h:
    // setStoreEntries()), a device's window's place given where the side has one, as view state outside the history.
    void giveHoldingsOf (Song& song, const juce::ValueTree& chosen)
    {
        std::vector<juce::ValueTree> tracks, owners, items;
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

        for (const auto& item : items)
        {
            const auto id = readDeviceElementID (item);
            DeviceStore entries;
            readHeldStoreOf (item, entries);
            setStoreEntries (song, id, entries);

            if (const auto window = item.hasType (schema::DEVICE) ? readWindowPlaceOf (item) : std::nullopt)
                setWindowPlace (song, id, *window);
        }
    }

    // A whole track chosen (ClashTrack: in the song's folders the path names, which the song holds): the song's, where it
    // holds it there, taken out with what it holds in the song's areas (eraseTrackHoldings(), then eraseTrack()), and the side
    // chosen's put in its place, where it has one: its values, its own and the tracks' in it, each read as the song holds one
    // (readHeldTrack()), in one action (setNewTracks()), their switches and view given before (giveLeavesOf()) and what they
    // hold after (giveHoldingsOf()). Refused, nothing written, where the song couldn't
    // hold the side's in its place: an element the path doesn't name, a track that reads as none, or one of an ID the song
    // holds outside the track it replaces (another track, a fader, a VCA, a device), or a folder whose tracks it couldn't hold.
    AreaChoice chooseWholeTrack (Song& song, const ClashTrack& track, const juce::ValueTree& chosen)
    {
        if (! track.placeHeld || ! track.id.isValid())
            return AreaChoice::refused;

        std::vector<TrackValue> made;
        std::vector<juce::ValueTree> elements;

        if (chosen.isValid())
        {
            if (! chosen.hasType (schema::TRACK) || readID (chosen[schema::trackID]) != track.id.getRaw())
                return AreaChoice::refused;

            collectTrackElements (chosen, elements);

            for (const auto& element : elements)
            {
                auto value = readHeldTrack (element);

                if (! value.has_value() || value->isMaster())
                    return AreaChoice::refused;

                made.push_back (std::move (*value));
            }

            made.front().parent = track.folder;

            // Each as the song would hold it, the track it replaces taken out first: no ID twice, none the song holds as
            // something else.
            auto trial = getHeldTracks (song);

            if (track.held.has_value())
                trial = withoutTrack (trial, track.id);

            std::set<std::uint64_t> ids;
            forEachIDHeld (trial, [&ids] (std::uint64_t id) { ids.insert (id); });

            for (const auto& value : made)
            {
                for (const auto id : { value.id.getRaw(), value.fader.getRaw(), value.vca.getRaw() })
                    if (id != 0 && (! ids.insert (id).second || findDevice (song, DeviceID::fromRaw (id)) != nullptr))
                        return AreaChoice::refused;

                if (auto next = withTrack (trial, value, -1); ! next.isSameAs (trial))
                    trial = std::move (next);
                else
                    return AreaChoice::refused;
            }
        }

        // The tracks the song holds in the one it replaces, by their IDs: what comes back under one of them takes its switches
        // in the step (giveLeavesOf()). And its place in its folder's list, which the side's takes.
        std::set<std::uint64_t> heldBefore;
        auto place = -1;

        if (track.held.has_value())
        {
            const auto tracks = getHeldTracks (song);
            if (const auto* list = tracks.findList (track.folder))
                for (size_t i = 0; i < list->size() && place < 0; ++i)
                    if ((*list)[i] == track.id)
                        place = (int) i;

            const std::function<void (TrackID)> collect = [&tracks, &heldBefore, &collect] (TrackID id)
            {
                heldBefore.insert (id.getRaw());

                if (const auto* list = tracks.findList (id))
                    for (const auto inside : *list)
                        collect (inside);
            };

            collect (track.id);
            eraseTrackHoldings (song, { track.id });
            eraseTrack (song, track.id);
        }

        // A clip on another track left the only one of its alias group (the song's track held the others, which the side's
        // holds in no group, or not at all) leaves the group, as deleteTracks() unlinks it.
        if (chosen.isValid())
        {
            giveLeavesOf (song, chosen, heldBefore);
            [[maybe_unused]] const auto added = setNewTracks (song, made, place);
            jassert (added); // what the trial above took
            giveHoldingsOf (song, chosen);
        }

        unlinkLoneClips (song);
        return AreaChoice::chosen;
    }

    // The whole master chosen: the side's (a song always has its master, so a side without one is refused) put in anew in
    // place of the song's (setMasterAnew()), what the song's held in the song's areas erased first (eraseTrackHoldings()),
    // its mode and row given before (giveLeavesOf()) and what it holds after (giveHoldingsOf()). Refused, nothing written,
    // where the side's reads as no master, or holds an ID (its own, its fader's, its VCA's) the song holds as something else.
    AreaChoice chooseWholeMaster (Song& song, const ClashTrack& track, const juce::ValueTree& chosen)
    {
        const auto value = chosen.hasType (schema::MASTER) ? readHeldTrack (chosen) : std::nullopt;

        if (! value.has_value() || ! value->isMaster() || ! value->id.isValid() || ! canHoldChosen (song, *track.held, *value))
            return AreaChoice::refused;

        eraseTrackHoldings (song, { track.id });
        giveLeavesOf (song, chosen, {});
        setMasterAnew (song, *value);
        giveHoldingsOf (song, chosen);
        unlinkLoneClips (song);
        return AreaChoice::chosen;
    }
} // namespace

AreaChoice chooseTrackClash (Song& song, const MergeClash& clash, int value, const MergeSides&)
{
    // Every clash whose path goes through the song's TRACKS or its MASTER that the areas asked before took none of
    // (VersionsModel.cpp: chooseInAreas(); a clip's, a device's, a modulator's, a send's are theirs): a property of a track's
    // or the master's own (chooseTrackProperty()), a whole track (chooseWholeTrack()) or the master (chooseWholeMaster()),
    // which only a commit's song from anyone makes a clash of (a merge of two songs Phazite holds keeps a track taken away on
    // one side and changed on the other, and merges the rest property by property: SongMerge.cpp). What is deeper in a track
    // (an element Phazite doesn't know) is refused: nothing through a TRACK or the MASTER is chosen in the song's tree.
    const auto found = findClashTrack (song, clash.path);

    if (! found.has_value())
        return AreaChoice::notTheArea;

    if (found->deeper || ! juce::isPositiveAndBelow (value, 3))
        return AreaChoice::refused;

    if (clash.property.isValid())
    {
        const juce::var* values[] = { &clash.base, &clash.ours, &clash.theirs };
        return chooseTrackProperty (song, *found, clash, *values[value]);
    }

    const juce::ValueTree* elements[] = { &clash.baseElement, &clash.oursElement, &clash.theirsElement };
    return found->master ? chooseWholeMaster (song, *found, *elements[value]) : chooseWholeTrack (song, *found, *elements[value]);
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
