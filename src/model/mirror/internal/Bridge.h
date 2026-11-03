// SPDX-License-Identifier: AGPL-3.0-or-later
// Tracktion's objects in the song's functions, while T6 lasts (docs/tracktion-migration.md): forms of the model's functions
// by IDs that take Tracktion's tracks, for the code that still holds them, each the song's function for the object's ID.
// New code calls the song's; what calls these moves to them area by area, and these go with the mirror (T6 step 9).
#pragma once

#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationValues.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/Tracktion.h"
#include "model/mixer/DeviceType.h"
#include "model/mixer/SidechainChoice.h"
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

//==============================================================================
// Devices (model/mixer/internal/Mixer.h). A device of Tracktion's (made with an Edit's plug-in cache, as a preset makes
// one) goes into Tracktion's list where the song's element of it goes, which is made from it (makeDeviceState()).
juce::Array<te::Plugin*> getDevices (const te::Track&);
juce::Array<te::Plugin*> getMasterDevices (const te::Edit&);
te::Plugin* getInstrument (const te::Track&);
juce::Array<te::Plugin*> getEffects (const te::Track&);
bool needsInstrument (const te::Track&);
te::Plugin* setInstrument (te::AudioTrack&, const te::Plugin::Ptr&);
te::Plugin* insertDevice (te::Track&, const te::Plugin::Ptr&, int index = -1);
te::Plugin* insertMasterDevice (te::Edit&, const te::Plugin::Ptr&, int index = -1);
void removeDevice (te::Plugin&);
void moveDevice (te::Plugin&, int newIndex);
void setDeviceOn (te::Plugin&, bool on);
bool isDeviceOn (const te::Plugin&); // Tracktion's own for a device the song hasn't (a preview's)
bool opensWindow (const te::Plugin&);
bool popsOut (const te::Plugin&);
bool hasMix (te::Plugin&);
float getMix (te::Plugin&);
void setMix (te::Plugin&, float mix);
void setDeviceSound (te::Plugin&, const juce::Identifier& property, const juce::String& value);
void rememberWindowPosition (te::Plugin&, juce::Point<int> topLeft);
juce::ValueTree findChainState (const te::Edit&, const te::Track*);

te::Plugin::Ptr createDevice (te::Edit&, const DeviceType&); // Tracktion's, on no channel
te::Plugin* addDeviceTo (te::Edit&, te::Track*, const DeviceType&, Selection*);
te::Plugin* placeDevice (te::Edit&, te::Track*, const te::Plugin::Ptr&, Selection*);
te::Plugin* chooseInstrument (te::AudioTrack&, const DeviceType&);
te::Plugin* addEffect (te::Edit&, te::Track*, const DeviceType&);
te::AudioTrack* addReturnWith (te::Track& sender, const DeviceType&);

void setMixedAsBus (te::FolderTrack&, bool);
te::Plugin* getVolumeDevice (te::Track&);
juce::String getVolumeParameterID (te::Track&);
void setChannelVolume (te::Track&, float decibels);
void setChannelPan (te::Track&, float pan);
float getChannelVolume (const te::Track&);
float getChannelPan (const te::Track&);
te::FolderTrack* getOutputBus (const te::Track&);
juce::Array<te::Track*> getChannelTracks (const te::Edit&);

int getReturnBus (const te::Track&);
juce::Array<te::AudioTrack*> getReturnTracks (const te::Edit&);
te::AudioTrack* findReturnTrack (const te::Edit&, int bus);
te::AudioTrack* addReturnTrack (te::Edit&, const juce::String& name, Selection*);
te::AudioTrack* addNewReturnTrack (te::Edit&, Selection*);
te::AudioTrack* addReturnWithEffect (te::Track& sender, const te::Plugin::Ptr& effect);
juce::Array<te::AuxSendPlugin*> getSends (const te::Track&);
te::AuxSendPlugin* addSend (te::Track&, int bus, bool preFader);
bool isPreFader (const te::AuxSendPlugin&);
void setPreFader (te::AuxSendPlugin&, bool);
void setSendLevel (te::AuxSendPlugin&, float decibels);
float getSendLevel (const te::AuxSendPlugin&);
te::AuxSendPlugin* addSendTo (te::Track&, int bus);
te::AuxSendPlugin* addSendToNewReturn (te::Track&);
void removeSend (te::AuxSendPlugin&);

// Containers (model/mixer/internal/Containers.h). A container of Tracktion's the song hasn't yet (a preset's) has its
// rack type's devices, in their order.
te::RackInstance* findContainerOf (const te::Plugin&);
juce::Array<te::Plugin*> getContainerDevices (const te::Plugin& container);
juce::ReferenceCountedArray<te::MacroParameter> getContainerMacros (const te::Plugin& container);
te::Track* getTrackOfContainer (const te::RackInstance&); // the master track for the master's
te::Track* getTrackOfContainerDevice (const te::Plugin&);
bool canGroupIntoContainer (const juce::Array<te::Plugin*>&);
te::RackInstance* groupIntoContainer (const juce::Array<te::Plugin*>&);
void ungroupContainer (te::RackInstance&);
void setContainerName (te::Plugin& container, const juce::String&);
te::Plugin* insertIntoContainer (te::RackInstance&, const te::Plugin::Ptr&, int index = -1);
void moveInContainer (te::Plugin&, int newIndex);
juce::String makeContainerName (const te::Edit&);

// Sidechains (model/mixer/internal/Sidechain.h).
juce::Array<te::Track*> getTracksFedBy (te::Track&);
bool hasSidechain (te::Plugin&);
juce::Array<te::AudioTrack*> getSidechainSources (te::Plugin&);
te::AudioTrack* getSidechainSource (te::Plugin&);
SidechainChoice describeSidechainSource (te::Plugin&);
std::vector<SidechainChoice> describeTracksToListenTo (te::Edit&, te::Track* listener);
std::vector<SidechainChoice> describeSidechainChoices (te::Plugin&);
void setSidechainSource (te::Plugin&, te::AudioTrack* source);
te::Plugin* duckUnder (te::AudioTrack& ducked, te::AudioTrack& key);
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
