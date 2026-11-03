// SPDX-License-Identifier: AGPL-3.0-or-later
// Tracktion's objects in the song's functions, while T6 lasts (docs/tracktion-migration.md): forms of the model's functions
// by IDs that take Tracktion's tracks, for the code that still holds them, each the song's function for the object's ID.
// New code calls the song's; what calls these moves to them area by area, and these go with the mirror (T6 step 9).
#pragma once

#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationValues.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/Tracktion.h"
#include "model/notes/NoteValues.h"

#include <optional>
#include <vector>

namespace phazite
{
//==============================================================================
// Tracks (model/arrangement/internal/Tracks.h).
juce::ValueTree getTrackList (te::Edit&);
juce::ValueTree findTrackList (const te::Edit&);
juce::ValueTree getMasterState (te::Edit&);
juce::ValueTree findTrackState (const te::Edit&, TrackID);
juce::ValueTree findTrackState (const te::Track&);
std::vector<juce::ValueTree> getTrackStates (const te::Edit&);

juce::String getTrackName (const te::Track&); // or Tracktion's name for a track the song hasn't
te::FolderTrack* getParentFolder (const te::Track&);
bool isReturnTrack (const te::Track&);
bool isTrackMuted (const te::Track&, bool orInMutedFolder);
bool isTrackSoloed (const te::Track&);
bool areAnyTracksSoloed (const te::Edit&);
void setTrackMuted (te::Track&, bool, juce::UndoManager* = nullptr);
void setTrackSoloed (te::Track&, bool);
void setTrackName (te::Track&, const juce::String&);
void setTrackColour (te::Track&, juce::Colour);
juce::Colour getTrackColour (const te::Track&, int row);
std::optional<int> getSavedTrackHeight (const te::Track&);
void setSavedTrackHeight (te::Track&, int height);
bool isCollapsed (const te::Track&);
void setCollapsed (te::Track&, bool);
bool isPinned (const te::Track&);
void setPinned (te::Track&, bool);
bool isHiddenInCollapsedFolder (const te::Track&);
void toggleSoloAlone (te::Track&);
bool isSilencedBySolo (const te::Track&);

juce::Array<te::Track*> getPlaylistTracks (const te::Edit&);
juce::Array<te::Track*> findTracks (const te::Edit&, const std::vector<TrackID>&);
std::vector<TrackID> getTrackIDs (const juce::Array<te::Track*>&);

te::AudioTrack* insertPlainTrack (te::Edit&, te::Track* after);
te::AudioTrack* insertTrack (te::Edit&, te::Track* after, Selection*);
te::AudioTrack* insertReturnTrack (te::Edit&, const juce::String& name, Selection*);
juce::Array<te::AudioTrack*> ensureTracks (te::Edit&, int count);
te::FolderTrack* insertFolder (te::Edit&, te::Track* after, Selection*);
te::FolderTrack* groupIntoFolder (const juce::Array<te::Track*>&, Selection*);
void moveTrackIntoFolder (te::Track&, te::FolderTrack&);
void ungroupFolder (te::FolderTrack&);
void ungroupFolders (te::Edit&, const std::vector<TrackID>& folders);
bool isAnyRowExpanded (te::Edit&);
void setAllRowsCollapsed (te::Edit&, bool collapsed);
void toggleAllRowsCollapsed (te::Edit&);
juce::Array<te::Track*> duplicateTracks (const juce::Array<te::Track*>&, Selection*);
void renameTrack (te::Track&, const juce::String& name);
te::Track* addTrackAtEnd (te::Edit&, bool folder, Selection*);
void setTrackColours (const juce::Array<te::Track*>&, juce::Colour);
void moveTracksAfter (const juce::Array<te::Track*>& moving, te::Track* previous);
bool canMoveTracksTo (const juce::Array<te::Track*>& moving, const te::Track& target, TrackDrop);
void moveTracksTo (const juce::Array<te::Track*>& moving, const te::Track& target, TrackDrop);
void deleteTracks (const juce::Array<te::Track*>&, Selection*);

//==============================================================================
// Buses (model/mixer/internal/Buses.h).
bool isBus (const te::Track&);
void makeBus (te::FolderTrack&);
void makePlainFolder (te::FolderTrack&);
void mixAsBus (te::FolderTrack&);

// Sidechains (model/mixer/internal/Sidechain.h).
void forgetSidechainSources (const juce::Array<te::Track*>&);

// Keys (model/notes/internal/Notes.h).
std::optional<Key> getTrackKey (const te::Track&);
void setTrackKey (te::Track&, const std::optional<Key>&);
void setTrackKeys (const juce::Array<te::Track*>&, const std::optional<Key>&);
void chooseTrackKey (te::Track&, const std::optional<Key>&);
void setLastKey (te::Edit&, const Key&);

// Automation lanes (model/automation/internal/Automation.h).
std::vector<AutomationLane> getLanes (te::Track&);
void showLane (te::Track&, const ParameterRef&);
void removeLane (te::Track&, const ParameterRef&);
void replaceLane (te::Track&, const ParameterRef& lane, const ParameterRef& replacement);
void setLaneHeight (te::Track&, const ParameterRef&, int height);
void setLaneCollapsed (te::Track&, const ParameterRef&, bool);
} // namespace phazite
