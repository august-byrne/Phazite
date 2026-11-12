// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/Toggles.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DrumKit.h"
#include "model/mixer/internal/Buses.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/notes/internal/Notes.h"

#include <algorithm>

namespace phazite
{
namespace
{
    // Only a folder holds tracks: the others' children (their clips and devices, many in a big song) aren't looked through.
    void collectTrackStates (const juce::ValueTree& parent, std::vector<juce::ValueTree>& states)
    {
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

    juce::ValueTree findIn (const juce::ValueTree& parent, TrackID id)
    {
        for (const auto& child : parent)
        {
            if (! child.hasType (schema::TRACK))
                continue;

            if (getTrackID (child) == id)
                return child;

            if (isFolderState (child))
                if (auto found = findIn (child, id); found.isValid())
                    return found;
        }

        return {};
    }

    // A new track's element, of a kind, with an ID of the song's (Song::makeID()), and its fader's: a folder starts as a bus
    // (design question 11).
    juce::ValueTree makeTrackState (Song& song, const char* kind, std::optional<juce::Colour> colour)
    {
        juce::ValueTree state (schema::TRACK);
        state.setProperty (schema::trackID, (juce::int64) song.makeID(), nullptr);
        state.setProperty (schema::trackKind, kind, nullptr);

        if (colour.has_value())
            state.setProperty (schema::trackColour, colour->toString(), nullptr);

        state.setProperty (schema::channelFader, (juce::int64) song.makeID(), nullptr);
        return state;
    }

    // Puts a track's element right after another track's, in that one's folder, or at the end of the list.
    void insertTrackState (Song& song, const juce::ValueTree& state, TrackID after)
    {
        if (const auto previous = findTrackState (song, after); previous.isValid() && ! previous.hasType (schema::MASTER))
        {
            auto parent = previous.getParent();
            parent.addChild (state, parent.indexOf (previous) + 1, &song.getUndoManager());
        }
        else
        {
            getTrackList (song).appendChild (state, &song.getUndoManager());
        }
    }

    // Moves a track's element to a place among a parent's children (a folder's element, or the list): before the child
    // at that index as the children are before the move.
    void moveTrackState (Song& song, const juce::ValueTree& state, juce::ValueTree parent, int index)
    {
        auto oldParent = state.getParent();
        auto* undo = &song.getUndoManager();

        if (oldParent == parent)
        {
            const auto oldIndex = parent.indexOf (state);
            const auto newIndex = index > oldIndex ? index - 1 : index;

            if (newIndex != oldIndex)
                parent.moveChild (oldIndex, juce::jlimit (0, parent.getNumChildren() - 1, newIndex), undo);

            return;
        }

        oldParent.removeChild (state, undo);
        parent.addChild (state, index, undo);

        // Into a bus that a sidechain on it listens to, say.
        forgetSidechainLoops (song);
    }

    // The tracks that move, in the playlist's order: those given, but those inside a folder that moves, which go with it.
    std::vector<juce::ValueTree> getMovingStates (const Song& song, const std::vector<TrackID>& tracks)
    {
        std::vector<juce::ValueTree> given, moving;

        for (const auto id : tracks)
            if (const auto state = findTrackState (song, id); state.hasType (schema::TRACK))
                given.push_back (state);

        const auto isGiven = [&given] (const juce::ValueTree& state)
        { return std::find (given.begin(), given.end(), state) != given.end(); };

        if (given.empty())
            return moving;

        for (const auto& state : getTrackStates (song))
        {
            auto inMovingFolder = false;

            for (auto parent = state.getParent(); parent.hasType (schema::TRACK) && ! inMovingFolder; parent = parent.getParent())
                inMovingFolder = isGiven (parent);

            if (isGiven (state) && ! inMovingFolder)
                moving.push_back (state);
        }

        return moving;
    }

    // Where moving tracks dropped at a track go: the element they go into, and the child of it they go before (invalid: after
    // its last), as the song is before they move. Nothing for a folder that would go into itself or a folder inside it.
    struct DropPlace
    {
        juce::ValueTree parent, before;
    };

    std::optional<DropPlace> findDropPlace (const std::vector<juce::ValueTree>& moving, const juce::ValueTree& target, TrackDrop drop)
    {
        const auto isMoving = [&moving] (const juce::ValueTree& state)
        { return std::find (moving.begin(), moving.end(), state) != moving.end(); };

        DropPlace place;

        if (! target.hasType (schema::TRACK))
            return {};

        if (drop == TrackDrop::into)
        {
            if (! isFolderState (target))
                return {};

            place.parent = target;
        }
        else
        {
            place.parent = target.getParent();

            // Before the first child from there on that doesn't move itself.
            for (auto i = place.parent.indexOf (target) + (drop == TrackDrop::after ? 1 : 0);
                 i < place.parent.getNumChildren() && ! place.before.isValid(); ++i)
                if (const auto child = place.parent.getChild (i); ! isMoving (child))
                    place.before = child;
        }

        if (! place.parent.isValid())
            return {};

        for (auto parent = place.parent; parent.hasType (schema::TRACK); parent = parent.getParent())
            if (isMoving (parent))
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
        const auto state = findTrackState (song, track);

        if (! state.hasType (schema::TRACK))
            return;

        if (const auto before = findTrackState (song, previous); before.hasType (schema::TRACK))
            moveTrackState (song, state, before.getParent(), before.getParent().indexOf (before) + 1);
        else
            moveTrackState (song, state, getTrackList (song), 0);
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
        const auto state = findTrackState (song, track);
        const auto parent = state.getParent();

        for (auto i = parent.indexOf (state) + direction; juce::isPositiveAndBelow (i, parent.getNumChildren()); i += direction)
            if (const auto sibling = parent.getChild (i); sibling.hasType (schema::TRACK))
                return getTrackID (sibling);

        return {};
    }

    bool isSoloedOrInSoloedFolder (const juce::ValueTree& state)
    {
        for (auto element = state; element.hasType (schema::TRACK); element = element.getParent())
            if ((bool) element[schema::trackSolo])
                return true;

        return false;
    }

    // The track whose drum sampler a pad's track plays a pad of (model/devices/internal/DrumKit.h), whose mute and solo cover
    // it as a folder's cover its tracks; none for any other track.
    juce::ValueTree findPadSamplerTrack (const Song& song, const juce::ValueTree& state)
    {
        if (! state.hasProperty (schema::trackPad))
            return {};

        const auto sampler =
            findTrackState (song, getDeviceChannel (song, DeviceID::fromRaw ((std::uint64_t) (juce::int64) state[schema::trackPadDevice])));
        return sampler != state ? sampler : juce::ValueTree();
    }

    bool isMutedOrInMutedFolder (const juce::ValueTree& state)
    {
        for (auto element = state; element.hasType (schema::TRACK); element = element.getParent())
            if ((bool) element[schema::trackMute])
                return true;

        return false;
    }

    // A track the song gives a key: the key set last on any of its tracks (model/notes/internal/Notes.h).
    TrackID madeWithLastKey (Song& song, TrackID id)
    {
        if (id.isValid())
            applyLastKey (song, id);

        return id;
    }
} // namespace

//==============================================================================
juce::ValueTree getTrackList (Song& song)
{
    getMasterState (song);
    return song.getState().getOrCreateChildWithName (schema::TRACKS, nullptr);
}

juce::ValueTree findTrackList (const Song& song)
{
    return song.getState().getChildWithName (schema::TRACKS);
}

juce::ValueTree getMasterState (Song& song)
{
    auto phazite = song.getState();

    if (auto master = phazite.getChildWithName (schema::MASTER); master.isValid())
        return master;

    // With IDs of the song's, for the master and its fader.
    juce::ValueTree master (schema::MASTER);
    master.setProperty (schema::trackID, (juce::int64) song.makeID(), nullptr);
    master.setProperty (schema::channelFader, (juce::int64) song.makeID(), nullptr);
    phazite.appendChild (master, nullptr);
    return master;
}

TrackID getMasterID (const Song& song)
{
    return getTrackID (song.getState().getChildWithName (schema::MASTER));
}

juce::ValueTree findTrackState (const Song& song, TrackID id)
{
    if (! id.isValid())
        return {};

    // The master's is its own element, with its view (its lanes).
    if (const auto master = song.getState().getChildWithName (schema::MASTER); getTrackID (master) == id)
        return master;

    return findIn (findTrackList (song), id);
}

std::vector<juce::ValueTree> getTrackStates (const Song& song)
{
    std::vector<juce::ValueTree> states;
    collectTrackStates (findTrackList (song), states);
    return states;
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
    return TrackID::fromRaw ((std::uint64_t) (juce::int64) state[schema::trackID]);
}

bool isFolderState (const juce::ValueTree& state)
{
    return state[schema::trackKind].toString() == schema::trackIsFolder;
}

bool isReturnState (const juce::ValueTree& state)
{
    return state[schema::trackKind].toString() == schema::trackIsReturn;
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

    if (! state.hasType (schema::TRACK))
        return state.hasType (schema::MASTER) ? juce::String ("Master") : juce::String();

    if (const auto name = state[schema::trackName].toString(); name.isNotEmpty() || isFolderState (state))
        return name;

    auto number = 1;

    for (const auto& other : getTrackStates (song))
    {
        if (other == state)
            break;

        if (! isFolderState (other))
            ++number;
    }

    return "Track " + juce::String (number);
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
        return state[schema::trackMute];

    // A pad's track is muted with its sampler's track, as a track is with its folder.
    return isMutedOrInMutedFolder (state) || isMutedOrInMutedFolder (findPadSamplerTrack (song, state));
}

bool isTrackSoloed (const Song& song, TrackID id)
{
    return findTrackState (song, id)[schema::trackSolo];
}

bool areAnyTracksSoloed (const Song& song)
{
    const auto states = getTrackStates (song);
    return std::any_of (states.begin(), states.end(), [] (const juce::ValueTree& state) { return (bool) state[schema::trackSolo]; });
}

void setTrackMuted (Song& song, TrackID id, bool muted, juce::UndoManager* undo)
{
    if (auto state = findTrackState (song, id); state.hasType (schema::TRACK) && (bool) state[schema::trackMute] != muted)
    {
        // Off is no property at all, as it is at first: unmuted again, the track is as it was, which a version's changes then
        // don't count (docs/versions.md).
        if (muted)
            state.setProperty (schema::trackMute, true, undo);
        else
            state.removeProperty (schema::trackMute, undo);
    }
}

void setTrackSoloed (Song& song, TrackID id, bool soloed)
{
    if (auto state = findTrackState (song, id); state.hasType (schema::TRACK) && (bool) state[schema::trackSolo] != soloed)
    {
        if (soloed)
            state.setProperty (schema::trackSolo, true, nullptr);
        else
            state.removeProperty (schema::trackSolo, nullptr);
    }
}

void setTrackName (Song& song, TrackID id, const juce::String& name)
{
    if (auto state = findTrackState (song, id); state.hasType (schema::TRACK))
        state.setProperty (schema::trackName, name, &song.getUndoManager());
}

void setTrackColour (Song& song, TrackID id, juce::Colour colour)
{
    if (auto state = findTrackState (song, id); state.hasType (schema::TRACK))
        state.setProperty (schema::trackColour, colour.toString(), &song.getUndoManager());
}

TrackID insertPlainTrack (Song& song, TrackID after)
{
    const auto state = makeTrackState (song, schema::trackIsTrack, std::nullopt);
    insertTrackState (song, state, after);
    return getTrackID (state);
}

//==============================================================================
juce::Colour getTrackColour (const Song& song, TrackID id, int row)
{
    if (const auto colour = juce::Colour::fromString (findTrackState (song, id)[schema::trackColour].toString()); ! colour.isTransparent())
        return colour;

    const auto& colours = getTrackColours();
    return colours[(size_t) (std::max (0, row) % (int) colours.size())].colour;
}

//==============================================================================
std::optional<int> getSavedTrackHeight (const Song& song, TrackID id)
{
    if (const auto* height = findTrackState (song, id).getPropertyPointer (schema::trackHeight))
        return (int) *height;

    return std::nullopt;
}

void setSavedTrackHeight (Song& song, TrackID id, int height)
{
    if (auto state = findTrackState (song, id); state.isValid())
        state.setProperty (schema::trackHeight, height, nullptr);
}

bool isCollapsed (const Song& song, TrackID id)
{
    return findTrackState (song, id).getProperty (schema::trackCollapsed, false);
}

void setCollapsed (Song& song, TrackID id, bool collapsed)
{
    if (auto state = findTrackState (song, id); state.isValid())
        state.setProperty (schema::trackCollapsed, collapsed, nullptr);
}

bool isPinned (const Song& song, TrackID id)
{
    return findTrackState (song, id).getProperty (schema::trackPinned, false);
}

void setPinned (Song& song, TrackID id, bool pinned)
{
    if (auto state = findTrackState (song, id); state.isValid())
        state.setProperty (schema::trackPinned, pinned, nullptr);
}

bool isHiddenInCollapsedFolder (const Song& song, TrackID id)
{
    for (auto folder = findTrackState (song, id).getParent(); folder.hasType (schema::TRACK); folder = folder.getParent())
        if ((bool) folder.getProperty (schema::trackCollapsed, false))
            return true;

    return false;
}

void toggleSoloAlone (Song& song, TrackID id)
{
    auto state = findTrackState (song, id);

    if (! state.hasType (schema::TRACK))
        return;

    auto alone = isTrackSoloed (song, id);

    for (auto other : getTrackStates (song))
        if (other != state && (bool) other[schema::trackSolo])
            alone = false;

    for (auto other : getTrackStates (song))
        if (other != state)
            other.removeProperty (schema::trackSolo, nullptr);

    if (alone)
        state.removeProperty (schema::trackSolo, nullptr);
    else
        state.setProperty (schema::trackSolo, true, nullptr);
}

bool isSilencedBySolo (const Song& song, TrackID id)
{
    const auto state = findTrackState (song, id);
    return areAnyTracksSoloed (song) && ! isSoloedOrInSoloedFolder (state) && ! isSoloedOrInSoloedFolder (findPadSamplerTrack (song, state))
           && ! isReturnState (state) && ! isTrackMuted (song, id, false);
}

//==============================================================================
TrackID insertTrack (Song& song, TrackID after, Selection* selection)
{
    const auto state = makeTrackState (song, schema::trackIsTrack, getNextColour (song, after));
    insertTrackState (song, state, after);
    const auto id = madeWithLastKey (song, getTrackID (state)); // the key chosen last in the song

    if (selection != nullptr)
        selection->selectOnly (id);

    return id;
}

TrackID insertReturnTrack (Song& song, const juce::String& name, Selection* selection)
{
    auto state = makeTrackState (song, schema::trackIsReturn, getNextColour (song, {}));
    state.setProperty (schema::trackName, name, nullptr);
    insertTrackState (song, state, {});
    const auto id = madeWithLastKey (song, getTrackID (state)); // as any new track gets it

    if (selection != nullptr)
        selection->selectOnly (id);

    return id;
}

std::vector<TrackID> ensureTracks (Song& song, int count)
{
    const auto getTracks = [&song]
    {
        std::vector<TrackID> tracks;

        for (const auto& state : getTrackStates (song))
            if (! isFolderState (state) && ! isReturnState (state))
                tracks.push_back (getTrackID (state));

        return tracks;
    };

    for (auto tracks = getTracks(); (int) tracks.size() < count; tracks = getTracks())
        insertPlainTrack (song, {});

    return getTracks();
}

TrackID insertFolder (Song& song, TrackID after, Selection* selection)
{
    auto state = makeTrackState (song, schema::trackIsFolder, getNextColour (song, after));
    state.setProperty (schema::trackName, "Folder", nullptr);
    insertTrackState (song, state, after);
    const auto folder = getTrackID (state);
    mixAsBus (song, folder); // a bus from the start (design question 11)

    if (selection != nullptr)
        selection->selectOnly (folder);

    return folder;
}

TrackID groupIntoFolder (Song& song, const std::vector<TrackID>& tracks, Selection* selection)
{
    const auto firstState = tracks.empty() ? juce::ValueTree() : findTrackState (song, tracks.front());

    if (! firstState.hasType (schema::TRACK))
        return {};

    // Where the first track is, in its own folder if it has one; in its color.
    const auto all = getTrackIDs (song);
    const auto row = (int) (std::find (all.begin(), all.end(), tracks.front()) - all.begin());
    auto state = makeTrackState (song, schema::trackIsFolder, getTrackColour (song, tracks.front(), row));
    state.setProperty (schema::trackName, "Folder", nullptr);
    auto parent = firstState.getParent();
    parent.addChild (state, parent.indexOf (firstState), &song.getUndoManager());
    const auto folder = getTrackID (state);
    mixAsBus (song, folder); // a bus from the start (design question 11)

    for (const auto track : tracks)
        moveTrackIntoFolder (song, track, folder);

    if (selection != nullptr)
        selection->selectOnly (folder);

    return folder;
}

void moveTrackIntoFolder (Song& song, TrackID track, TrackID folder)
{
    const auto state = findTrackState (song, track);
    auto folderState = findTrackState (song, folder);

    // A folder can't go into itself, or into a folder inside it.
    if (! state.hasType (schema::TRACK) || ! isFolderState (folderState) || state == folderState || folderState.isAChildOf (state))
        return;

    moveTrackState (song, state, folderState, folderState.getNumChildren());
}

void ungroupFolder (Song& song, TrackID folder)
{
    const auto folderState = findTrackState (song, folder);

    if (! isFolderState (folderState))
        return;

    // Its tracks go where it was, in their order, and it goes, with the sidechains that listened to it.
    forgetSidechainsOfBus (song, folder);
    auto parent = folderState.getParent();
    auto place = parent.indexOf (folderState) + 1;

    for (int i = 0; i < folderState.getNumChildren();)
    {
        if (const auto child = folderState.getChild (i); child.hasType (schema::TRACK))
            moveTrackState (song, child, parent, place++);
        else
            ++i;
    }

    parent.removeChild (folderState, &song.getUndoManager());
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

std::vector<TrackID> duplicateTracks (Song& song, const std::vector<TrackID>& tracks, Selection* selection)
{
    std::vector<TrackID> copies;

    for (const auto track : tracks)
    {
        const auto original = findTrackState (song, track);

        if (! original.hasType (schema::TRACK))
            continue;

        // The copy holds what its devices hold now (a plug-in's own settings), every item in it the song's with an ID of its
        // own (its tracks, devices, sends, modulators and clips, its clips' notes as they are, in their alias groups), with
        // its own automation, right after the original. A frozen track's copy plays live: the frozen file is the original's.
        song.storeDeviceStates (track.getRaw());
        auto copy = original.createCopy();
        const auto copied = giveNewIDs (song, copy);

        std::vector<juce::ValueTree> inside { copy };
        collectTrackStates (copy, inside);

        // Nor does a copy of a pad's track play the pad, which plays on the original.
        for (auto state : inside)
            for (const auto* property : { &schema::trackFrozen, &schema::trackPadDevice, &schema::trackPad })
                state.removeProperty (*property, nullptr);

        auto parent = original.getParent();
        parent.addChild (copy, parent.indexOf (original) + 1, &song.getUndoManager());
        copyCurves (song, copied);
        copies.push_back (getTrackID (copy));
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
    auto state = findTrackState (song, track);

    if (! state.hasType (schema::TRACK) || trimmed == state[schema::trackName].toString())
        return;

    const UndoStep step (song, "Rename Track");
    state.setProperty (schema::trackName, trimmed, &song.getUndoManager());
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
        if (auto state = findTrackState (song, track); state.hasType (schema::TRACK))
            state.setProperty (schema::trackColour, colour.toString(), &song.getUndoManager());
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
    const auto moving = getMovingStates (song, tracks);
    return ! moving.empty() && findDropPlace (moving, findTrackState (song, target), drop).has_value();
}

void moveTracksTo (Song& song, const std::vector<TrackID>& tracks, TrackID target, TrackDrop drop)
{
    const auto moving = getMovingStates (song, tracks);
    const auto place = moving.empty() ? std::nullopt : findDropPlace (moving, findTrackState (song, target), drop);

    if (! place.has_value())
        return;

    const UndoStep step (song, moving.size() > 1 ? "Move Tracks" : "Move Track");

    // Each goes before the same child, which doesn't move, so that they keep their order.
    for (const auto& state : moving)
        moveTrackState (song, state, place->parent,
                        place->before.isValid() ? place->parent.indexOf (place->before) : place->parent.getNumChildren());
}

void deleteTracks (Song& song, const std::vector<TrackID>& tracks, Selection* selection)
{
    // A folder takes the tracks inside it along, so those aren't deleted on their own.
    std::vector<TrackID> outermost;

    for (const auto track : tracks)
    {
        if (! findTrackState (song, track).hasType (schema::TRACK))
            continue;

        auto inDeletedFolder = false;

        for (auto folder = getParentFolder (song, track); folder.isValid() && ! inDeletedFolder; folder = getParentFolder (song, folder))
            inDeletedFolder = std::find (tracks.begin(), tracks.end(), folder) != tracks.end();

        if (! inDeletedFolder)
            outermost.push_back (track);
    }

    if (outermost.empty())
        return;

    const auto all = getTrackIDs (song);
    const auto row = (int) (std::find (all.begin(), all.end(), outermost.front()) - all.begin());
    forgetSidechainSources (song, outermost);

    for (const auto track : outermost)
        if (const auto state = findTrackState (song, track); state.isValid())
            state.getParent().removeChild (state, &song.getUndoManager());

    if (selection != nullptr)
        if (const auto remaining = getTrackIDs (song); ! remaining.empty())
            selection->selectOnly (remaining[(size_t) juce::jmin (row, (int) remaining.size() - 1)]);
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
