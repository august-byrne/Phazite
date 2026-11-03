// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/Toggles.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DeviceStates.h"
#include "model/mirror/internal/Mirror.h"
#include "model/mixer/internal/Buses.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/modulation/internal/Modulation.h"
#include "model/notes/internal/Notes.h"

#include <algorithm>

namespace phazite
{
namespace
{
    juce::UndoManager* getUndo (te::Edit& edit)
    {
        return &getSong (edit).getUndoManager();
    }

    void collectTrackStates (const juce::ValueTree& parent, std::vector<juce::ValueTree>& states)
    {
        for (const auto& child : parent)
        {
            if (child.hasType (schema::TRACK))
            {
                states.push_back (child);
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

            if (auto found = findIn (child, id); found.isValid())
                return found;
        }

        return {};
    }

    // A new track's element, of a kind, with an ID of the song's (Song::makeID()), and its fader's: a folder starts as a bus
    // (design question 11).
    juce::ValueTree makeTrackState (te::Edit& edit, const char* kind, juce::Colour colour)
    {
        juce::ValueTree state (schema::TRACK);
        state.setProperty (schema::trackID, (juce::int64) getSong (edit).makeID(), nullptr);
        state.setProperty (schema::trackKind, kind, nullptr);
        state.setProperty (schema::trackColour, colour.toString(), nullptr);
        state.setProperty (schema::channelFader, (juce::int64) getSong (edit).makeID(), nullptr);
        return state;
    }

    // Puts a track's element right after another track's, in that one's folder, or at the end of the list.
    void insertTrackState (te::Edit& edit, const juce::ValueTree& state, const te::Track* after)
    {
        if (const auto previous = after != nullptr ? findTrackState (*after) : juce::ValueTree(); previous.isValid())
        {
            auto parent = previous.getParent();
            parent.addChild (state, parent.indexOf (previous) + 1, getUndo (edit));
        }
        else
        {
            getTrackList (edit).appendChild (state, getUndo (edit));
        }
    }

    // Moves a track's element to a place among a parent's children (a folder's element, or the list): before the child
    // at that index as the children are before the move.
    void moveTrackState (te::Edit& edit, const juce::ValueTree& state, juce::ValueTree parent, int index)
    {
        auto oldParent = state.getParent();

        if (oldParent == parent)
        {
            const auto oldIndex = parent.indexOf (state);
            const auto newIndex = index > oldIndex ? index - 1 : index;

            if (newIndex != oldIndex)
                parent.moveChild (oldIndex, juce::jlimit (0, parent.getNumChildren() - 1, newIndex), getUndo (edit));

            return;
        }

        oldParent.removeChild (state, getUndo (edit));
        parent.addChild (state, index, getUndo (edit));
    }

    // The tracks that move, in the playlist's order: those given, but those inside a folder that moves, which go with it.
    std::vector<juce::ValueTree> getMovingStates (const juce::Array<te::Track*>& tracks)
    {
        std::vector<juce::ValueTree> given, moving;

        for (auto* track : tracks)
            if (const auto state = findTrackState (*track); state.isValid())
                given.push_back (state);

        const auto isGiven = [&given] (const juce::ValueTree& state)
        { return std::find (given.begin(), given.end(), state) != given.end(); };

        if (given.empty())
            return moving;

        for (const auto& state : getTrackStates (tracks.getFirst()->edit))
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

        if (! target.isValid() || ! place.parent.isValid())
            return {};

        for (auto parent = place.parent; parent.hasType (schema::TRACK); parent = parent.getParent())
            if (isMoving (parent))
                return {};

        return place;
    }

    // The tracks that have a row in the playlist, or can have one: its tracks and folders, and the returns and the master,
    // whose rows show while they have automation lanes.
    juce::Array<te::Track*> getRowTracks (te::Edit& edit)
    {
        auto tracks = getPlaylistTracks (edit);

        if (auto* master = edit.getMasterTrack())
            tracks.add (master);

        return tracks;
    }

    // Moves a track right after another one, in its folder, or else first in the list.
    void moveTrackAfter (te::Track& track, const te::Track* previous)
    {
        const auto state = findTrackState (track);

        if (! state.isValid())
            return;

        if (const auto before = previous != nullptr ? findTrackState (*previous) : juce::ValueTree(); before.isValid())
            moveTrackState (track.edit, state, before.getParent(), before.getParent().indexOf (before) + 1);
        else
            moveTrackState (track.edit, state, getTrackList (track.edit), 0);
    }

    // The palette's next color after the track a new one goes below (the last track if that is nullptr),
    // so that neighbours differ.
    juce::Colour getNextColour (te::Edit& edit, te::Track* after)
    {
        const auto tracks = getPlaylistTracks (edit);
        const auto& colours = getTrackColours();
        size_t next = 0;

        if (auto* above = after != nullptr ? after : tracks.getLast())
        {
            const auto aboveColour = getTrackColour (*above, tracks.indexOf (above));

            for (size_t i = 0; i < colours.size(); ++i)
                if (colours[i].colour == aboveColour)
                    next = (i + 1) % colours.size();
        }

        return colours[next].colour;
    }

    // The playlist track before (direction -1) or after (1) this one in its folder, or among the top-level tracks.
    te::Track* getSibling (te::Track& track, int direction)
    {
        const auto state = findTrackState (track);
        const auto parent = state.getParent();

        for (auto i = parent.indexOf (state) + direction; juce::isPositiveAndBelow (i, parent.getNumChildren()); i += direction)
            if (const auto sibling = parent.getChild (i); sibling.hasType (schema::TRACK))
                return findTrack (track.edit, sibling);

        return nullptr;
    }

    bool isSoloedOrInSoloedFolder (const juce::ValueTree& state)
    {
        for (auto element = state; element.hasType (schema::TRACK); element = element.getParent())
            if ((bool) element[schema::trackSolo])
                return true;

        return false;
    }

    // Each track's element in a copy of a track's, given the ID its copy in Tracktion has: the copy's tracks are in the
    // same order as the original's, and its devices', containers', sends', modulators' and macros' elements name the copy's.
    // Its clips get IDs of their own, as copyClipState() gives a clip's copy, with the same notes, in the same alias group.
    void giveCopiedIDs (juce::ValueTree& copied, const te::Track& copy, const te::EditItemID::IDMap& copiedDevices)
    {
        copied.setProperty (schema::trackID, (juce::int64) copy.itemID.getRawID(), nullptr);
        giveCopiedDeviceIDs (copied, copiedDevices);
        giveCopiedModulatorIDs (copied, copiedDevices);

        for (auto child : copied)
            if (child.hasType (schema::CLIP))
                child.setProperty (schema::clipID, (juce::int64) getSong (copy.edit).makeID(), nullptr);

        if (auto* folder = dynamic_cast<const te::FolderTrack*> (&copy))
        {
            const auto subTracks = folder->getAllSubTracks (false);
            auto index = 0;

            for (auto child : copied)
                if (child.hasType (schema::TRACK) && index < subTracks.size())
                    giveCopiedIDs (child, *subTracks[index++], copiedDevices);
        }
    }
} // namespace

//==============================================================================
juce::ValueTree getTrackList (te::Edit& edit)
{
    auto phazite = getSong (edit).getState();
    getMasterState (edit);
    return phazite.getOrCreateChildWithName (schema::TRACKS, nullptr);
}

juce::ValueTree findTrackList (const Song& song)
{
    return song.getState().getChildWithName (schema::TRACKS);
}

juce::ValueTree findTrackList (const te::Edit& edit)
{
    return findPhaziteState (edit).getChildWithName (schema::TRACKS);
}

juce::ValueTree getMasterState (te::Edit& edit)
{
    auto phazite = getSong (edit).getState();

    if (auto master = phazite.getChildWithName (schema::MASTER); master.isValid())
        return master;

    // With the master's ID and its fader's, those of Tracktion's master track and fader, which it makes with the Edit (and
    // createEditFromSong() with the song's IDs).
    juce::ValueTree master (schema::MASTER);

    if (auto* track = edit.getMasterTrack())
        master.setProperty (schema::trackID, (juce::int64) track->itemID.getRawID(), nullptr);

    if (const auto fader = edit.getMasterVolumePlugin())
        master.setProperty (schema::channelFader, (juce::int64) fader->itemID.getRawID(), nullptr);

    phazite.appendChild (master, nullptr);
    return master;
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

te::Track* findTrack (const te::Edit& edit, const juce::ValueTree& state)
{
    return state.isValid() ? te::findTrackForID (edit, toTracktion (getTrackID (state))) : nullptr;
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

juce::String getTrackName (const Song& song, TrackID id)
{
    const auto state = findTrackState (song, id);

    if (! state.isValid())
        return {};

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

juce::String getTrackName (const te::Track& track)
{
    const auto& song = getSong (track.edit);
    return findTrackState (song, getTrackID (track)).isValid() ? getTrackName (song, getTrackID (track)) : track.getName();
}

bool isReturnTrack (const Song& song, TrackID id)
{
    return isReturnState (findTrackState (song, id));
}

TrackID getParentFolder (const Song& song, TrackID id)
{
    return getTrackID (findTrackState (song, id).getParent());
}

te::FolderTrack* getParentFolder (const te::Track& track)
{
    return dynamic_cast<te::FolderTrack*> (findTrack (track.edit, findTrackState (track).getParent()));
}

bool isTrackMuted (const Song& song, TrackID id, bool orInMutedFolder)
{
    for (auto state = findTrackState (song, id); state.hasType (schema::TRACK); state = state.getParent())
        if ((bool) state[schema::trackMute] || ! orInMutedFolder)
            return state[schema::trackMute];

    return false;
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
    if (auto state = findTrackState (song, id); state.isValid() && (bool) state[schema::trackMute] != muted)
    {
        state.setProperty (schema::trackMute, muted, undo);
        song.reconcile (Song::Part::tracks);
    }
}

void setTrackSoloed (Song& song, TrackID id, bool soloed)
{
    if (auto state = findTrackState (song, id); state.isValid() && (bool) state[schema::trackSolo] != soloed)
    {
        state.setProperty (schema::trackSolo, soloed, nullptr);
        song.reconcile (Song::Part::tracks);
    }
}

void setTrackName (Song& song, TrackID id, const juce::String& name)
{
    if (auto state = findTrackState (song, id); state.isValid())
        state.setProperty (schema::trackName, name, &song.getUndoManager());

    song.reconcile (Song::Part::tracks);
}

void setTrackColour (Song& song, TrackID id, juce::Colour colour)
{
    if (auto state = findTrackState (song, id); state.isValid())
        state.setProperty (schema::trackColour, colour.toString(), &song.getUndoManager());

    song.reconcile (Song::Part::tracks);
}

te::AudioTrack* insertPlainTrack (te::Edit& edit, te::Track* after)
{
    juce::ValueTree state (schema::TRACK);
    state.setProperty (schema::trackID, (juce::int64) getSong (edit).makeID(), nullptr);
    state.setProperty (schema::trackKind, schema::trackIsTrack, nullptr);
    state.setProperty (schema::channelFader, (juce::int64) getSong (edit).makeID(), nullptr);
    insertTrackState (edit, state, after);
    mirror::reconcileTracks (edit);
    return dynamic_cast<te::AudioTrack*> (findTrack (edit, state));
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
juce::Array<te::Track*> getPlaylistTracks (const te::Edit& edit)
{
    juce::Array<te::Track*> tracks;

    for (const auto& state : getTrackStates (getSong (edit)))
        if (auto* track = findTrack (edit, state))
            tracks.add (track);

    return tracks;
}

juce::Array<te::Track*> getSelectedTracks (const Selection& selection)
{
    juce::Array<te::Track*> selected;

    if (auto* edit = selection.getEdit())
        for (auto* track : getPlaylistTracks (*edit))
            if (selection.isSelected (track))
                selected.add (track);

    return selected;
}

juce::Array<te::Track*> getSelectedOutermostTracks (const Selection& selection)
{
    juce::Array<te::Track*> tracks;

    for (auto* track : getSelectedTracks (selection))
    {
        auto inSelectedFolder = false;

        for (auto* folder = getParentFolder (*track); folder != nullptr && ! inSelectedFolder; folder = getParentFolder (*folder))
            inSelectedFolder = selection.isSelected (folder);

        if (! inSelectedFolder)
            tracks.add (track);
    }

    return tracks;
}

te::TrackInsertPoint getInsertPointAfter (te::Track& track)
{
    return { track.getParentTrack(), &track };
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

    if (! state.isValid())
        return;

    auto alone = isTrackSoloed (song, id);

    for (auto other : getTrackStates (song))
        if (other != state && (bool) other[schema::trackSolo])
            alone = false;

    for (auto other : getTrackStates (song))
        if (other != state)
            other.setProperty (schema::trackSolo, false, nullptr);

    state.setProperty (schema::trackSolo, ! alone, nullptr);
    song.reconcile (Song::Part::tracks);
}

bool isSilencedBySolo (const Song& song, TrackID id)
{
    const auto state = findTrackState (song, id);
    return areAnyTracksSoloed (song) && ! isSoloedOrInSoloedFolder (state) && ! isReturnState (state) && ! isTrackMuted (song, id, false);
}

//==============================================================================
te::AudioTrack* insertTrack (te::Edit& edit, te::Track* after, Selection* selection)
{
    const auto state = makeTrackState (edit, schema::trackIsTrack, getNextColour (edit, after));
    insertTrackState (edit, state, after);
    mirror::reconcileTracks (edit);
    auto* track = dynamic_cast<te::AudioTrack*> (findTrack (edit, state));

    if (track == nullptr)
        return nullptr;

    applyLastKey (*track); // the key chosen last in the song

    if (selection != nullptr)
        selection->selectOnly (track);

    return track;
}

te::AudioTrack* insertReturnTrack (te::Edit& edit, const juce::String& name, Selection* selection)
{
    auto state = makeTrackState (edit, schema::trackIsReturn, getNextColour (edit, nullptr));
    state.setProperty (schema::trackName, name, nullptr);
    insertTrackState (edit, state, nullptr);
    mirror::reconcileTracks (edit);
    auto* track = dynamic_cast<te::AudioTrack*> (findTrack (edit, state));

    if (track == nullptr)
        return nullptr;

    applyLastKey (*track); // as any new track gets it

    if (selection != nullptr)
        selection->selectOnly (track);

    return track;
}

void recordFirstTrack (te::Edit& edit)
{
    auto list = getTrackList (edit);
    const auto tracks = te::getAudioTracks (edit);

    if (getTrackStates (edit).empty() && ! tracks.isEmpty())
    {
        juce::ValueTree state (schema::TRACK);
        state.setProperty (schema::trackID, (juce::int64) tracks.getFirst()->itemID.getRawID(), nullptr);
        state.setProperty (schema::trackKind, schema::trackIsTrack, nullptr);

        if (auto* fader = getVolumeDevice (*tracks.getFirst()))
            state.setProperty (schema::channelFader, (juce::int64) fader->itemID.getRawID(), nullptr);

        list.appendChild (state, nullptr);
    }
}

juce::Array<te::AudioTrack*> ensureTracks (te::Edit& edit, int count)
{
    const auto getTracks = [&edit]
    {
        juce::Array<te::AudioTrack*> tracks;

        for (const auto& state : getTrackStates (edit))
            if (! isFolderState (state) && ! isReturnState (state))
                if (auto* track = dynamic_cast<te::AudioTrack*> (findTrack (edit, state)))
                    tracks.add (track);

        return tracks;
    };

    for (auto tracks = getTracks(); tracks.size() < count; tracks = getTracks())
        if (insertPlainTrack (edit, nullptr) == nullptr)
            break;

    return getTracks();
}

te::FolderTrack* insertFolder (te::Edit& edit, te::Track* after, Selection* selection)
{
    auto state = makeTrackState (edit, schema::trackIsFolder, getNextColour (edit, after));
    state.setProperty (schema::trackName, "Folder", nullptr);
    insertTrackState (edit, state, after);
    mirror::reconcileTracks (edit);
    auto* folder = dynamic_cast<te::FolderTrack*> (findTrack (edit, state));

    if (folder == nullptr)
        return nullptr;

    mixAsBus (*folder); // a bus from the start (design question 11)

    if (selection != nullptr)
        selection->selectOnly (folder);

    return folder;
}

te::FolderTrack* groupIntoFolder (const juce::Array<te::Track*>& tracks, Selection* selection)
{
    if (tracks.isEmpty())
        return nullptr;

    // Where the first track is, in its own folder if it has one; in its color.
    auto& first = *tracks.getFirst();
    auto& edit = first.edit;
    const auto firstState = findTrackState (first);
    auto state = makeTrackState (edit, schema::trackIsFolder, getTrackColour (first, getPlaylistTracks (edit).indexOf (&first)));
    state.setProperty (schema::trackName, "Folder", nullptr);
    auto parent = firstState.getParent();
    parent.addChild (state, parent.indexOf (firstState), getUndo (edit));
    mirror::reconcileTracks (edit);
    auto* folder = dynamic_cast<te::FolderTrack*> (findTrack (edit, state));

    if (folder == nullptr)
        return nullptr;

    mixAsBus (*folder); // a bus from the start (design question 11)

    for (auto* track : tracks)
        moveTrackIntoFolder (*track, *folder);

    if (selection != nullptr)
        selection->selectOnly (folder);

    return folder;
}

void moveTrackIntoFolder (te::Track& track, te::FolderTrack& folder)
{
    const auto state = findTrackState (track);
    auto folderState = findTrackState (folder);

    // A folder can't go into itself, or into a folder inside it.
    if (! state.isValid() || ! folderState.isValid() || state == folderState || folderState.isAChildOf (state))
        return;

    moveTrackState (track.edit, state, folderState, folderState.getNumChildren());
    mirror::reconcileTracks (track.edit);
}

void ungroupFolder (te::FolderTrack& folder)
{
    auto& edit = folder.edit;
    const auto folderState = findTrackState (folder);

    if (! folderState.isValid())
        return;

    // Its tracks go where it was, in their order, and it goes.
    auto parent = folderState.getParent();
    auto place = parent.indexOf (folderState) + 1;

    for (int i = 0; i < folderState.getNumChildren();)
    {
        if (const auto child = folderState.getChild (i); child.hasType (schema::TRACK))
            moveTrackState (edit, child, parent, place++);
        else
            ++i;
    }

    parent.removeChild (folderState, getUndo (edit));
    mirror::reconcileTracks (edit);
}

//==============================================================================
bool hasSelectedTracks (const Selection& selection)
{
    return ! getSelectedTracks (selection).isEmpty();
}

juce::Array<te::FolderTrack*> getSelectedFolders (const Selection& selection)
{
    juce::Array<te::FolderTrack*> folders;

    for (auto* track : getSelectedTracks (selection))
        if (auto* folder = dynamic_cast<te::FolderTrack*> (track))
            folders.add (folder);

    return folders;
}

bool hasSelectedFolders (const Selection& selection)
{
    return ! getSelectedFolders (selection).isEmpty();
}

juce::Array<te::Track*> getSelectedTracksOrAll (const Selection& selection)
{
    auto tracks = getSelectedTracks (selection);

    if (tracks.isEmpty())
        if (auto* edit = selection.getEdit())
            tracks = getPlaylistTracks (*edit);

    return tracks;
}

te::AudioTrack* insertTrackAfterSelection (Selection& selection)
{
    auto* edit = selection.getEdit();
    return edit != nullptr ? insertTrack (*edit, getSelectedTracks (selection).getLast(), &selection) : nullptr;
}

te::FolderTrack* insertFolderAfterSelection (Selection& selection)
{
    auto* edit = selection.getEdit();
    return edit != nullptr ? insertFolder (*edit, getSelectedTracks (selection).getLast(), &selection) : nullptr;
}

void ungroupFolders (const juce::Array<te::FolderTrack*>& folders)
{
    for (auto* folder : folders)
        ungroupFolder (*folder);
}

void selectNeighbourTrack (Selection& selection, int direction)
{
    auto* edit = selection.getEdit();

    if (edit == nullptr)
        return;

    const auto tracks = getPlaylistTracks (*edit);
    const auto selected = getSelectedTracks (selection);

    if (tracks.isEmpty())
        return;

    if (selected.isEmpty())
    {
        selection.selectOnly (direction > 0 ? tracks.getFirst() : tracks.getLast());
        return;
    }

    const auto row = tracks.indexOf (direction > 0 ? selected.getLast() : selected.getFirst()) + direction;
    selection.selectOnly (tracks[juce::jlimit (0, tracks.size() - 1, row)]);
}

void moveSelectedTracks (Selection& selection, int direction)
{
    auto* edit = selection.getEdit();
    auto tracks = getSelectedOutermostTracks (selection);

    if (edit == nullptr)
        return;

    // The track nearest to where they move goes first.
    if (direction > 0)
        std::reverse (tracks.begin(), tracks.end());

    for (auto* track : tracks)
    {
        auto* neighbour = getSibling (*track, direction);

        if (neighbour == nullptr || selection.isSelected (neighbour))
            continue;

        if (direction < 0)
            moveTrackAfter (*neighbour, track); // the one above goes below
        else
            moveTrackAfter (*track, neighbour);
    }

    mirror::reconcileTracks (*edit);
}

bool areAllTracksMuted (const juce::Array<te::Track*>& tracks)
{
    return areAllOn (tracks, [] (te::Track* track) { return isTrackMuted (*track, false); });
}

void toggleTracksMuted (const juce::Array<te::Track*>& tracks)
{
    toggleAll (
        tracks, [] (te::Track* track) { return isTrackMuted (*track, false); },
        [] (te::Track* track, bool on) { setTrackMuted (*track, on); });
}

bool areAllTracksSoloed (const juce::Array<te::Track*>& tracks)
{
    return areAllOn (tracks, [] (te::Track* track) { return isTrackSoloed (*track); });
}

void toggleTracksSoloed (const juce::Array<te::Track*>& tracks)
{
    toggleAll (
        tracks, [] (te::Track* track) { return isTrackSoloed (*track); }, [] (te::Track* track, bool on) { setTrackSoloed (*track, on); });
}

bool areAllTracksCollapsed (const juce::Array<te::Track*>& tracks)
{
    return areAllOn (tracks, [] (te::Track* track) { return isCollapsed (*track); });
}

void toggleTracksCollapsed (const juce::Array<te::Track*>& tracks)
{
    toggleAll (
        tracks, [] (te::Track* track) { return isCollapsed (*track); }, [] (te::Track* track, bool on) { setCollapsed (*track, on); });
}

bool areAllTracksPinned (const juce::Array<te::Track*>& tracks)
{
    return areAllOn (tracks, [] (te::Track* track) { return isPinned (*track); });
}

void toggleTracksPinned (const juce::Array<te::Track*>& tracks)
{
    toggleAll (tracks, [] (te::Track* track) { return isPinned (*track); }, [] (te::Track* track, bool on) { setPinned (*track, on); });
}

bool isAnyRowExpanded (te::Edit& edit)
{
    for (auto* track : getRowTracks (edit))
    {
        // A return's or the master's own row shows only with lanes.
        const auto lanes = getLanes (*track);
        const auto hasRow = ! track->isMasterTrack() && ! isReturnTrack (*track);

        if ((hasRow || ! lanes.empty()) && ! isCollapsed (*track))
            return true;

        if (std::any_of (lanes.begin(), lanes.end(), [] (const AutomationLane& lane) { return ! lane.collapsed; }))
            return true;
    }

    return false;
}

void setAllRowsCollapsed (te::Edit& edit, bool collapsed)
{
    for (auto* track : getRowTracks (edit))
    {
        setCollapsed (*track, collapsed);

        for (const auto& lane : getLanes (*track))
            setLaneCollapsed (*track, lane.parameter, collapsed);
    }
}

void toggleAllRowsCollapsed (te::Edit& edit)
{
    setAllRowsCollapsed (edit, isAnyRowExpanded (edit));
}

juce::Array<te::Track*> duplicateTracks (const juce::Array<te::Track*>& tracks, Selection* selection)
{
    juce::Array<te::Track*> copies;

    for (auto* track : tracks)
    {
        // The copy takes the devices' states as they are now (Tracktion stores those of the track's own devices as it
        // copies, but through the undo manager: model/devices/internal/DeviceStates.h).
        storeDeviceStates (*track);
        const auto original = findTrackState (*track);

        // Tracktion copies the track's devices, giving the copy, and the tracks in it, IDs of their own, and its containers
        // hold devices of their own; the song's track list gets the same copy, with those IDs, right after it. The song's
        // clips in the copy, with IDs of their own, replace the clips Tracktion copied as the clips are placed again.
        if (auto copy = track->edit.copyTrack (track, getInsertPointAfter (*track)); copy != nullptr && original.isValid())
        {
            const auto inContainers = copyContainers (*track, *copy);
            auto copiedDevices = inContainers;
            mapCopiedDevices (*track, *copy, copiedDevices);
            mapCopiedModulators (*track, *copy, copiedDevices);
            auto copied = original.createCopy();
            giveCopiedIDs (copied, *copy, copiedDevices);
            auto parent = original.getParent();
            parent.addChild (copied, parent.indexOf (original) + 1, getUndo (track->edit));
            copyLanesAndCurves (*track, *copy, inContainers); // its lanes and automation are its own devices'
            copies.add (copy.get());
        }
    }

    if (! tracks.isEmpty())
    {
        mirror::reconcileTracks (tracks.getFirst()->edit);
        mirror::reconcileClips (tracks.getFirst()->edit);
    }

    if (selection != nullptr)
    {
        selection->deselectAll();

        for (auto* copy : copies)
            selection->addToSelection (copy);
    }

    return copies;
}

void renameTrack (te::Track& track, const juce::String& name)
{
    const auto trimmed = name.trim();
    auto state = findTrackState (track);

    if (! state.isValid() || trimmed == state[schema::trackName].toString())
        return;

    const UndoStep step (track.edit, "Rename Track");
    state.setProperty (schema::trackName, trimmed, getUndo (track.edit));
    mirror::reconcileTracks (track.edit);
}

te::Track* addTrackAtEnd (te::Edit& edit, bool folder, Selection* selection)
{
    const UndoStep step (edit, folder ? "Add Folder" : "Add Track");

    if (folder)
        return insertFolder (edit, nullptr, selection);

    return insertTrack (edit, nullptr, selection);
}

void setTrackColours (const juce::Array<te::Track*>& tracks, juce::Colour colour)
{
    if (tracks.isEmpty())
        return;

    auto& edit = tracks.getFirst()->edit;
    const UndoStep step (edit, tracks.size() > 1 ? "Change Track Colors" : "Change Track Color");

    for (auto* track : tracks)
        findTrackState (*track).setProperty (schema::trackColour, colour.toString(), getUndo (edit));

    mirror::reconcileTracks (edit);
}

void moveTracksAfter (const juce::Array<te::Track*>& moving, te::Track* previous)
{
    if (moving.isEmpty())
        return;

    auto& edit = moving.getFirst()->edit;
    const UndoStep step (edit, moving.size() > 1 ? "Move Tracks" : "Move Track");

    for (auto* track : moving)
    {
        moveTrackAfter (*track, previous);
        previous = track;
    }

    mirror::reconcileTracks (edit);
}

bool canMoveTracksTo (const juce::Array<te::Track*>& tracks, const te::Track& target, TrackDrop drop)
{
    const auto moving = getMovingStates (tracks);
    return ! moving.empty() && findDropPlace (moving, findTrackState (target), drop).has_value();
}

void moveTracksTo (const juce::Array<te::Track*>& tracks, const te::Track& target, TrackDrop drop)
{
    const auto moving = getMovingStates (tracks);
    const auto place = moving.empty() ? std::nullopt : findDropPlace (moving, findTrackState (target), drop);

    if (! place.has_value())
        return;

    auto& edit = target.edit;
    const UndoStep step (edit, moving.size() > 1 ? "Move Tracks" : "Move Track");

    // Each goes before the same child, which doesn't move, so that they keep their order.
    for (const auto& state : moving)
        moveTrackState (edit, state, place->parent,
                        place->before.isValid() ? place->parent.indexOf (place->before) : place->parent.getNumChildren());

    mirror::reconcileTracks (edit);
}

void deleteTracks (const juce::Array<te::Track*>& tracks, Selection* selection)
{
    // A folder takes the tracks inside it along, so those aren't deleted on their own.
    juce::Array<te::Track*> outermost;

    for (auto* track : tracks)
    {
        auto inDeletedFolder = false;

        for (auto* folder = getParentFolder (*track); folder != nullptr && ! inDeletedFolder; folder = getParentFolder (*folder))
            inDeletedFolder = tracks.contains (folder);

        if (! inDeletedFolder)
            outermost.add (track);
    }

    if (outermost.isEmpty())
        return;

    auto& edit = outermost.getFirst()->edit;
    const auto row = getPlaylistTracks (edit).indexOf (outermost.getFirst());
    forgetSidechainSources (outermost);

    for (auto* track : outermost)
        if (const auto state = findTrackState (*track); state.isValid())
            state.getParent().removeChild (state, getUndo (edit));

    mirror::reconcileTracks (edit);

    if (selection != nullptr)
        if (const auto remaining = getPlaylistTracks (edit); ! remaining.isEmpty())
            selection->selectOnly (remaining[juce::jmin (row, remaining.size() - 1)]);
}

//==============================================================================
void addTrackCommands (CommandRegistry& registry, Selection& selection)
{
    const auto undoTarget = undoTargetOf (selection);
    const auto hasEdit = [&selection] { return selection.getEdit() != nullptr; };

    registry.add ({ .id = "track.add",
                    .name = "Add Track",
                    .category = "Track",
                    .description = "Adds an empty track below the selected one, or at the bottom.",
                    .synonyms = { "new track", "insert track", "create" },
                    .perform = [&selection] { insertTrackAfterSelection (selection); },
                    .isEnabled = hasEdit,
                    .undoTarget = undoTarget });

    // Folders.
    registry.add ({ .id = "track.addFolder",
                    .name = "Add Folder",
                    .category = "Track",
                    .description = "Adds an empty folder below the selected track, or at the bottom.",
                    .synonyms = { "new folder", "group", "bus" },
                    .perform = [&selection] { insertFolderAfterSelection (selection); },
                    .isEnabled = hasEdit,
                    .undoTarget = undoTarget });

    registry.add ({ .id = "track.groupIntoFolder",
                    .name = "Group into Folder",
                    .category = "Track",
                    .description = "Puts the selected tracks into a new folder. A folder becomes a bus when it holds an effect.",
                    .synonyms = { "group", "folder", "bus", "submix" },
                    .perform = [&selection] { groupIntoFolder (getSelectedOutermostTracks (selection), &selection); },
                    .isEnabled = [&selection] { return hasSelectedTracks (selection); },
                    .undoTarget = undoTarget });

    registry.add ({ .id = "track.ungroup",
                    .name = "Ungroup Folder",
                    .category = "Track",
                    .description = "Takes the tracks out of the selected folders, and deletes the folders.",
                    .synonyms = { "remove folder", "ungroup", "unfolder" },
                    .perform = [&selection] { ungroupFolders (getSelectedFolders (selection)); },
                    .isEnabled = [&selection] { return hasSelectedFolders (selection); },
                    .undoTarget = undoTarget });

    // Collapsing and pinning change the view, so they aren't undo steps.
    registry.add ({ .id = "track.collapse",
                    .name = "Collapse Track",
                    .category = "Track",
                    .description = "Makes the selected tracks thin rows that show only their names, or full height again. A "
                                   "collapsed folder hides its tracks.",
                    .synonyms = { "fold", "unfold", "expand", "hide", "minimize", "folder" },
                    .perform = [&selection] { toggleTracksCollapsed (getSelectedTracks (selection)); },
                    .isEnabled = [&selection] { return hasSelectedTracks (selection); },
                    .isTicked = [&selection] { return areAllTracksCollapsed (getSelectedTracks (selection)); } });

    registry.add ({ .id = "track.collapseAll",
                    .name = "Collapse All",
                    .category = "Track",
                    .description = "Makes every track, folder and automation lane a thin row, or, when they all are, shows them "
                                   "all at full height again.",
                    .synonyms = { "fold all", "unfold all", "expand all", "collapse everything", "minimize all" },
                    .perform = withEdit (selection, [] (te::Edit& edit) { toggleAllRowsCollapsed (edit); }),
                    .isEnabled = hasEdit,
                    .currentName = [&selection]
                    {
                        auto* edit = selection.getEdit();
                        return edit == nullptr || isAnyRowExpanded (*edit) ? juce::String ("Collapse All") : juce::String ("Expand All");
                    } });

    registry.add ({ .id = "track.pin",
                    .name = "Pin Track",
                    .category = "Track",
                    .description = "Keeps the selected tracks in view at the top of the playlist while the others scroll.",
                    .synonyms = { "stick", "keep visible", "unpin" },
                    .perform = [&selection] { toggleTracksPinned (getSelectedTracks (selection)); },
                    .isEnabled = [&selection] { return hasSelectedTracks (selection); },
                    .isTicked = [&selection] { return areAllTracksPinned (getSelectedTracks (selection)); } });

    registry.add ({ .id = "track.selectPrevious",
                    .name = "Select Track Above",
                    .category = "Track",
                    .description = "Selects the track above the selected one.",
                    .synonyms = { "previous track", "up" },
                    .perform = [&selection] { selectNeighbourTrack (selection, -1); },
                    .isEnabled = hasEdit,
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "track.selectNext",
                    .name = "Select Track Below",
                    .category = "Track",
                    .description = "Selects the track below the selected one.",
                    .synonyms = { "next track", "down" },
                    .perform = [&selection] { selectNeighbourTrack (selection, 1); },
                    .isEnabled = hasEdit,
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

    // Tracktion keeps mute and solo out of the undo history, as many DAWs do.
    registry.add ({ .id = "track.mute",
                    .name = "Mute Track",
                    .category = "Track",
                    .description = "Silences the selected tracks, or unmutes them.",
                    .synonyms = { "silence", "unmute" },
                    .perform = [&selection] { toggleTracksMuted (getSelectedTracks (selection)); },
                    .isEnabled = [&selection] { return hasSelectedTracks (selection); },
                    .isTicked = [&selection] { return areAllTracksMuted (getSelectedTracks (selection)); } });

    registry.add ({ .id = "track.solo",
                    .name = "Solo Track",
                    .category = "Track",
                    .description = "Plays only the selected tracks and any others that are soloed, or ends their solo.",
                    .synonyms = { "isolate", "unsolo" },
                    .perform = [&selection] { toggleTracksSoloed (getSelectedTracks (selection)); },
                    .isEnabled = [&selection] { return hasSelectedTracks (selection); },
                    .isTicked = [&selection] { return areAllTracksSoloed (getSelectedTracks (selection)); } });
}
} // namespace phazite
