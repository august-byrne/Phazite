// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Bridge.h"
#include "internal/Mirror.h"
#include "model/automation/internal/Automation.h"
#include "model/mixer/internal/Buses.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/notes/internal/Notes.h"

namespace phazite
{
namespace
{
    Song& songOf (const te::Track& track)
    {
        return getSong (track.edit);
    }

    TrackID idOf (const te::Track* track)
    {
        return track != nullptr ? getTrackID (*track) : TrackID();
    }

    template <typename Type>
    Type* find (const te::Edit& edit, TrackID id)
    {
        return dynamic_cast<Type*> (mirror::findTrack (edit, id));
    }
} // namespace

//==============================================================================
juce::ValueTree getTrackList (te::Edit& edit)
{
    return getTrackList (getSong (edit));
}

juce::ValueTree findTrackList (const te::Edit& edit)
{
    return findPhaziteState (edit).getChildWithName (schema::TRACKS);
}

juce::ValueTree getMasterState (te::Edit& edit)
{
    return getMasterState (getSong (edit));
}

juce::ValueTree findTrackState (const te::Edit& edit, TrackID id)
{
    return findTrackState (getSong (edit), id);
}

juce::ValueTree findTrackState (const te::Track& track)
{
    return findTrackState (songOf (track), getTrackID (track));
}

std::vector<juce::ValueTree> getTrackStates (const te::Edit& edit)
{
    return getTrackStates (getSong (edit));
}

juce::String getTrackName (const te::Track& track)
{
    return findTrackState (track).isValid() ? getTrackName (songOf (track), getTrackID (track)) : track.getName();
}

te::FolderTrack* getParentFolder (const te::Track& track)
{
    return find<te::FolderTrack> (track.edit, getParentFolder (songOf (track), getTrackID (track)));
}

bool isReturnTrack (const te::Track& track)
{
    return isReturnTrack (songOf (track), getTrackID (track));
}

bool isTrackMuted (const te::Track& track, bool orInMutedFolder)
{
    return isTrackMuted (songOf (track), getTrackID (track), orInMutedFolder);
}

bool isTrackSoloed (const te::Track& track)
{
    return isTrackSoloed (songOf (track), getTrackID (track));
}

bool areAnyTracksSoloed (const te::Edit& edit)
{
    return areAnyTracksSoloed (getSong (edit));
}

void setTrackMuted (te::Track& track, bool muted, juce::UndoManager* undo)
{
    setTrackMuted (songOf (track), getTrackID (track), muted, undo);
}

void setTrackSoloed (te::Track& track, bool soloed)
{
    setTrackSoloed (songOf (track), getTrackID (track), soloed);
}

void setTrackName (te::Track& track, const juce::String& name)
{
    setTrackName (songOf (track), getTrackID (track), name);
}

void setTrackColour (te::Track& track, juce::Colour colour)
{
    setTrackColour (songOf (track), getTrackID (track), colour);
}

juce::Colour getTrackColour (const te::Track& track, int row)
{
    return getTrackColour (songOf (track), getTrackID (track), row);
}

std::optional<int> getSavedTrackHeight (const te::Track& track)
{
    return getSavedTrackHeight (songOf (track), getTrackID (track));
}

void setSavedTrackHeight (te::Track& track, int height)
{
    setSavedTrackHeight (songOf (track), getTrackID (track), height);
}

bool isCollapsed (const te::Track& track)
{
    return isCollapsed (songOf (track), getTrackID (track));
}

void setCollapsed (te::Track& track, bool collapsed)
{
    setCollapsed (songOf (track), getTrackID (track), collapsed);
}

bool isPinned (const te::Track& track)
{
    return isPinned (songOf (track), getTrackID (track));
}

void setPinned (te::Track& track, bool pinned)
{
    setPinned (songOf (track), getTrackID (track), pinned);
}

bool isHiddenInCollapsedFolder (const te::Track& track)
{
    return isHiddenInCollapsedFolder (songOf (track), getTrackID (track));
}

void toggleSoloAlone (te::Track& track)
{
    toggleSoloAlone (songOf (track), getTrackID (track));
}

bool isSilencedBySolo (const te::Track& track)
{
    return isSilencedBySolo (songOf (track), getTrackID (track));
}

juce::Array<te::Track*> getPlaylistTracks (const te::Edit& edit)
{
    return mirror::findTracks (edit, getTrackIDs (getSong (edit)));
}

juce::Array<te::Track*> findTracks (const te::Edit& edit, const std::vector<TrackID>& ids)
{
    return mirror::findTracks (edit, ids);
}

std::vector<TrackID> getTrackIDs (const juce::Array<te::Track*>& tracks)
{
    std::vector<TrackID> ids;

    for (auto* track : tracks)
        if (track != nullptr)
            ids.push_back (getTrackID (*track));

    return ids;
}

te::AudioTrack* insertPlainTrack (te::Edit& edit, te::Track* after)
{
    return find<te::AudioTrack> (edit, insertPlainTrack (getSong (edit), idOf (after)));
}

te::AudioTrack* insertTrack (te::Edit& edit, te::Track* after, Selection* selection)
{
    return find<te::AudioTrack> (edit, insertTrack (getSong (edit), idOf (after), selection));
}

te::AudioTrack* insertReturnTrack (te::Edit& edit, const juce::String& name, Selection* selection)
{
    return find<te::AudioTrack> (edit, insertReturnTrack (getSong (edit), name, selection));
}

juce::Array<te::AudioTrack*> ensureTracks (te::Edit& edit, int count)
{
    juce::Array<te::AudioTrack*> tracks;

    for (const auto id : ensureTracks (getSong (edit), count))
        if (auto* track = find<te::AudioTrack> (edit, id))
            tracks.add (track);

    return tracks;
}

te::FolderTrack* insertFolder (te::Edit& edit, te::Track* after, Selection* selection)
{
    return find<te::FolderTrack> (edit, insertFolder (getSong (edit), idOf (after), selection));
}

te::FolderTrack* groupIntoFolder (const juce::Array<te::Track*>& tracks, Selection* selection)
{
    if (tracks.isEmpty())
        return nullptr;

    auto& edit = tracks.getFirst()->edit;
    return find<te::FolderTrack> (edit, groupIntoFolder (getSong (edit), getTrackIDs (tracks), selection));
}

void moveTrackIntoFolder (te::Track& track, te::FolderTrack& folder)
{
    moveTrackIntoFolder (songOf (track), getTrackID (track), getTrackID (folder));
}

void ungroupFolder (te::FolderTrack& folder)
{
    ungroupFolder (songOf (folder), getTrackID (folder));
}

void ungroupFolders (te::Edit& edit, const std::vector<TrackID>& folders)
{
    ungroupFolders (getSong (edit), folders);
}

bool isAnyRowExpanded (te::Edit& edit)
{
    return isAnyRowExpanded (getSong (edit));
}

void setAllRowsCollapsed (te::Edit& edit, bool collapsed)
{
    setAllRowsCollapsed (getSong (edit), collapsed);
}

void toggleAllRowsCollapsed (te::Edit& edit)
{
    toggleAllRowsCollapsed (getSong (edit));
}

juce::Array<te::Track*> duplicateTracks (const juce::Array<te::Track*>& tracks, Selection* selection)
{
    if (tracks.isEmpty())
        return {};

    auto& edit = tracks.getFirst()->edit;
    return mirror::findTracks (edit, duplicateTracks (getSong (edit), getTrackIDs (tracks), selection));
}

void renameTrack (te::Track& track, const juce::String& name)
{
    renameTrack (songOf (track), getTrackID (track), name);
}

te::Track* addTrackAtEnd (te::Edit& edit, bool folder, Selection* selection)
{
    return mirror::findTrack (edit, addTrackAtEnd (getSong (edit), folder, selection));
}

void setTrackColours (const juce::Array<te::Track*>& tracks, juce::Colour colour)
{
    if (! tracks.isEmpty())
        setTrackColours (songOf (*tracks.getFirst()), getTrackIDs (tracks), colour);
}

void moveTracksAfter (const juce::Array<te::Track*>& moving, te::Track* previous)
{
    if (! moving.isEmpty())
        moveTracksAfter (songOf (*moving.getFirst()), getTrackIDs (moving), idOf (previous));
}

bool canMoveTracksTo (const juce::Array<te::Track*>& moving, const te::Track& target, TrackDrop drop)
{
    return canMoveTracksTo (songOf (target), getTrackIDs (moving), getTrackID (target), drop);
}

void moveTracksTo (const juce::Array<te::Track*>& moving, const te::Track& target, TrackDrop drop)
{
    moveTracksTo (songOf (target), getTrackIDs (moving), getTrackID (target), drop);
}

void deleteTracks (const juce::Array<te::Track*>& tracks, Selection* selection)
{
    if (! tracks.isEmpty())
        deleteTracks (songOf (*tracks.getFirst()), getTrackIDs (tracks), selection);
}

//==============================================================================
bool isBus (const te::Track& track)
{
    return isBus (songOf (track), getTrackID (track));
}

void makeBus (te::FolderTrack& folder)
{
    makeBus (songOf (folder), getTrackID (folder));
}

void makePlainFolder (te::FolderTrack& folder)
{
    makePlainFolder (songOf (folder), getTrackID (folder));
}

void mixAsBus (te::FolderTrack& folder)
{
    mixAsBus (songOf (folder), getTrackID (folder));
}

void forgetSidechainSources (const juce::Array<te::Track*>& tracks)
{
    if (! tracks.isEmpty())
        forgetSidechainSources (songOf (*tracks.getFirst()), getTrackIDs (tracks));
}

std::optional<Key> getTrackKey (const te::Track& track)
{
    return getTrackKey (songOf (track), getTrackID (track));
}

void setTrackKey (te::Track& track, const std::optional<Key>& key)
{
    setTrackKey (songOf (track), getTrackID (track), key);
}

void setTrackKeys (const juce::Array<te::Track*>& tracks, const std::optional<Key>& key)
{
    if (! tracks.isEmpty())
        setTrackKeys (songOf (*tracks.getFirst()), getTrackIDs (tracks), key);
}

void chooseTrackKey (te::Track& track, const std::optional<Key>& key)
{
    chooseTrackKey (songOf (track), getTrackID (track), key);
}

void setLastKey (te::Edit& edit, const Key& key)
{
    setLastKey (getSong (edit), key);
}

std::vector<AutomationLane> getLanes (te::Track& track)
{
    return getLanes (songOf (track), getTrackID (track));
}

void showLane (te::Track& track, const ParameterRef& parameter)
{
    showLane (songOf (track), getTrackID (track), parameter);
}

void removeLane (te::Track& track, const ParameterRef& parameter)
{
    removeLane (songOf (track), getTrackID (track), parameter);
}

void replaceLane (te::Track& track, const ParameterRef& lane, const ParameterRef& replacement)
{
    replaceLane (songOf (track), getTrackID (track), lane, replacement);
}

void setLaneHeight (te::Track& track, const ParameterRef& parameter, int height)
{
    setLaneHeight (songOf (track), getTrackID (track), parameter, height);
}

void setLaneCollapsed (te::Track& track, const ParameterRef& parameter, bool collapsed)
{
    setLaneCollapsed (songOf (track), getTrackID (track), parameter, collapsed);
}
} // namespace phazite
