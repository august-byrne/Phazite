// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Bridge.h"
#include "internal/Mirror.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Import.h"
#include "model/automation/internal/Automation.h"
#include "model/devices/internal/DevicePlugin.h"
#include "model/devices/internal/Devices.h"
#include "model/mixer/internal/Buses.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
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

//==============================================================================
namespace
{
    Song& songOf (const te::Plugin& device)
    {
        return getSong (device.edit);
    }

    template <typename Type = te::Plugin>
    Type* findDevice (const te::Edit& edit, DeviceID id)
    {
        return dynamic_cast<Type*> (mirror::findDevice (edit, id));
    }

    juce::Array<te::Plugin*> findDevices (const te::Edit& edit, const std::vector<DeviceID>& ids)
    {
        juce::Array<te::Plugin*> devices;

        for (const auto id : ids)
            if (auto* device = mirror::findDevice (edit, id))
                devices.add (device);

        return devices;
    }

    std::vector<DeviceID> getDeviceIDs (const juce::Array<te::Plugin*>& devices)
    {
        std::vector<DeviceID> ids;

        for (auto* device : devices)
            ids.push_back (device != nullptr ? getDeviceID (*device) : DeviceID());

        return ids;
    }

    // A device of Tracktion's goes into a channel's list where its element goes among the song's, which is made from it
    // (instruments first); one the song can't take there goes again.
    te::Plugin* insertInto (te::PluginList& list, TrackID channel, const te::Plugin::Ptr& plugin, int deviceIndex)
    {
        if (plugin == nullptr)
            return nullptr;

        const auto index = isInstrument (*plugin) ? 0 : deviceIndex;
        list.insertPlugin (plugin, mirror::getDevicePlace (list, index), nullptr);

        if (! list.contains (plugin.get()))
            return nullptr;

        if (! insertDeviceState (getSong (plugin->edit), channel, makeDeviceState (*plugin), index).isValid())
        {
            plugin->deleteFromParent();
            return nullptr;
        }

        return plugin.get();
    }

    // The dry and wet levels a device of Tracktion's has, as a mix from 0 to 1 (Mixer.h: getMix()).
    float getTracktionMix (te::Plugin& device)
    {
        const auto parameters = getMixParameters (device);

        if (parameters.size() == 1)
            return parameters.getFirst()->getCurrentValue();

        if (parameters.size() == 2)
        {
            const auto dry = parameters[0]->getCurrentValue(), wet = parameters[1]->getCurrentValue();
            return wet + dry > 0.0f ? juce::jlimit (0.0f, 1.0f, wet / (wet + dry)) : 1.0f;
        }

        return 1.0f;
    }
} // namespace

juce::Array<te::Plugin*> getDevices (const te::Track& track)
{
    return mirror::findDevices (track.edit, getTrackID (track));
}

juce::Array<te::Plugin*> getMasterDevices (const te::Edit& edit)
{
    return mirror::findDevices (edit, {});
}

te::Plugin* getInstrument (const te::Track& track)
{
    return mirror::findDevice (track.edit, getInstrument (songOf (track), getTrackID (track)));
}

juce::Array<te::Plugin*> getEffects (const te::Track& track)
{
    return findDevices (track.edit, getEffects (songOf (track), getTrackID (track)));
}

bool needsInstrument (const te::Track& track)
{
    return needsInstrument (songOf (track), getTrackID (track));
}

te::Plugin* setInstrument (te::AudioTrack& track, const te::Plugin::Ptr& plugin)
{
    auto& song = songOf (track);
    const auto id = getTrackID (track);

    if (plugin == nullptr || ! isInstrument (*plugin) || isReturnTrack (song, id))
        return nullptr;

    // The old one's element out (what its channel's devices hold stored first, for an undo), and the new one in its place.
    if (auto old = findDeviceState (song, getInstrument (song, id)); old.isValid())
    {
        song.storeDeviceStates (id.getRaw());
        old.getParent().removeChild (old, &song.getUndoManager());
    }

    return insertInto (track.pluginList, id, plugin, 0);
}

te::Plugin* insertDevice (te::Track& track, const te::Plugin::Ptr& plugin, int index)
{
    return insertInto (track.pluginList, getTrackID (track), plugin, index);
}

te::Plugin* insertMasterDevice (te::Edit& edit, const te::Plugin::Ptr& plugin, int index)
{
    return insertInto (edit.getMasterPluginList(), {}, plugin, index);
}

void removeDevice (te::Plugin& device)
{
    removeDevice (songOf (device), getDeviceID (device));
}

void moveDevice (te::Plugin& device, int newIndex)
{
    moveDevice (songOf (device), getDeviceID (device), newIndex);
}

void setDeviceOn (te::Plugin& device, bool on)
{
    if (findDeviceState (device.edit, getDeviceID (device)).isValid())
        setDeviceOn (songOf (device), getDeviceID (device), on);
    else
        device.setEnabled (on);
}

bool isDeviceOn (const te::Plugin& device)
{
    if (findDeviceState (device.edit, getDeviceID (device)).isValid())
        return isDeviceOn (songOf (device), getDeviceID (device));

    return device.isEnabled();
}

bool opensWindow (const te::Plugin& device)
{
    return dynamic_cast<const te::ExternalPlugin*> (&device) != nullptr;
}

bool popsOut (const te::Plugin& device)
{
    return dynamic_cast<const DevicePlugin*> (&device) != nullptr;
}

bool hasMix (te::Plugin& device)
{
    if (findDeviceState (device.edit, getDeviceID (device)).isValid())
        return hasMix (songOf (device), getDeviceID (device));

    return ! getMixParameters (device).isEmpty();
}

float getMix (te::Plugin& device)
{
    if (findDeviceState (device.edit, getDeviceID (device)).isValid())
        return getMix (songOf (device), getDeviceID (device));

    return getTracktionMix (device);
}

void setMix (te::Plugin& device, float mix)
{
    if (findDeviceState (device.edit, getDeviceID (device)).isValid())
    {
        setMix (songOf (device), getDeviceID (device), mix);
        return;
    }

    // One of Tracktion's the song hasn't yet (a preset's): its own parameters, which its element is made from.
    const auto wet = juce::jlimit (0.0f, 1.0f, mix);
    const auto parameters = getMixParameters (device);

    if (parameters.size() == 1)
    {
        setDeviceValue (*parameters.getFirst(), wet);
    }
    else if (parameters.size() == 2)
    {
        setDeviceValue (*parameters[0], 1.0f - wet);
        setDeviceValue (*parameters[1], wet);
    }
}

void setDeviceSound (te::Plugin& device, const juce::Identifier& property, const juce::String& value)
{
    if (findDeviceState (device.edit, getDeviceID (device)).isValid())
    {
        setDeviceSound (songOf (device), getDeviceID (device), property, value);
        return;
    }

    // One of Tracktion's the song hasn't (a preview's): its own state.
    if (value.isNotEmpty())
        device.state.setProperty (property, value, &device.edit.getUndoManager());
    else
        device.state.removeProperty (property, &device.edit.getUndoManager());
}

void rememberWindowPosition (te::Plugin& device, juce::Point<int> topLeft)
{
    rememberWindowPosition (songOf (device), getDeviceID (device), topLeft);
}

juce::ValueTree findChainState (const te::Edit& edit, const te::Track* track)
{
    return findChainState (getSong (edit), idOf (track));
}

te::Plugin::Ptr createDevice (te::Edit& edit, const DeviceType& type)
{
    if (te::PluginManager::isBuiltInPlugin (type.description))
        return edit.getPluginCache().createNewPlugin (type.description.fileOrIdentifier, type.description);

    return edit.getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, type.description);
}

te::Plugin* addDeviceTo (te::Edit& edit, te::Track* track, const DeviceType& type, Selection* selection)
{
    return mirror::findDevice (edit, addDeviceTo (getSong (edit), idOf (track), type, selection));
}

te::Plugin* placeDevice (te::Edit& edit, te::Track* track, const te::Plugin::Ptr& device, Selection* selection)
{
    if (device == nullptr)
        return nullptr;

    if (! isInstrument (*device))
        return track != nullptr ? insertDevice (*track, device) : insertMasterDevice (edit, device);

    // An instrument would silence a track's audio clips, so a track with audio gets one next to it instead, and so does a
    // folder, a return or none (placeDevice() in Mixer.h).
    auto& song = getSong (edit);
    auto* audioTrack = dynamic_cast<te::AudioTrack*> (track);

    if (audioTrack == nullptr || isReturnTrack (song, getTrackID (*audioTrack))
        || hasAudioClips (song, getClips (song, getTrackID (*audioTrack))))
    {
        const auto made = insertTrack (song, idOf (track), selection);
        audioTrack = find<te::AudioTrack> (edit, made);

        if (audioTrack == nullptr)
            return nullptr;

        setTrackName (song, made, device->getName());

        if (const auto colour = getColourForName (device->getName()))
            setTrackColour (song, made, *colour);
    }

    return setInstrument (*audioTrack, device);
}

te::Plugin* chooseInstrument (te::AudioTrack& track, const DeviceType& type)
{
    return mirror::findDevice (track.edit, chooseInstrument (songOf (track), getTrackID (track), type));
}

te::Plugin* addEffect (te::Edit& edit, te::Track* track, const DeviceType& type)
{
    return mirror::findDevice (edit, addEffect (getSong (edit), idOf (track), type));
}

te::AudioTrack* addReturnWith (te::Track& sender, const DeviceType& type)
{
    return find<te::AudioTrack> (sender.edit, addReturnWith (songOf (sender), getTrackID (sender), type));
}

void setMixedAsBus (te::FolderTrack& folder, bool bus)
{
    setMixedAsBus (songOf (folder), getTrackID (folder), bus);
}

te::Plugin* getVolumeDevice (te::Track& track)
{
    return mirror::getFader (track);
}

juce::String getVolumeParameterID (te::Track& track)
{
    return getVolumeParameterID (songOf (track), getTrackID (track));
}

void setChannelVolume (te::Track& track, float decibels)
{
    setChannelVolume (songOf (track), getTrackID (track), decibels);
}

void setChannelPan (te::Track& track, float pan)
{
    setChannelPan (songOf (track), getTrackID (track), pan);
}

float getChannelVolume (const te::Track& track)
{
    return getChannelVolume (songOf (track), getTrackID (track));
}

float getChannelPan (const te::Track& track)
{
    return getChannelPan (songOf (track), getTrackID (track));
}

te::FolderTrack* getOutputBus (const te::Track& track)
{
    return find<te::FolderTrack> (track.edit, getOutputBus (songOf (track), getTrackID (track)));
}

juce::Array<te::Track*> getChannelTracks (const te::Edit& edit)
{
    return mirror::findTracks (edit, getChannelTracks (getSong (edit)));
}

int getReturnBus (const te::Track& track)
{
    return mirror::getReturnBus (track);
}

juce::Array<te::AudioTrack*> getReturnTracks (const te::Edit& edit)
{
    juce::Array<te::AudioTrack*> returns;

    for (const auto track : getReturnTracks (getSong (edit)))
        if (auto* audio = find<te::AudioTrack> (edit, track))
            returns.add (audio);

    return returns;
}

te::AudioTrack* findReturnTrack (const te::Edit& edit, int bus)
{
    return mirror::findReturnTrack (edit, bus);
}

te::AudioTrack* addReturnTrack (te::Edit& edit, const juce::String& name, Selection* selection)
{
    return find<te::AudioTrack> (edit, addReturnTrack (getSong (edit), name, selection));
}

te::AudioTrack* addNewReturnTrack (te::Edit& edit, Selection* selection)
{
    return find<te::AudioTrack> (edit, addNewReturnTrack (getSong (edit), selection));
}

te::AudioTrack* addReturnWithEffect (te::Track& sender, const te::Plugin::Ptr& effect)
{
    if (effect == nullptr || isInstrument (*effect))
        return nullptr;

    // Named after the effect: "Reverb", or "Reverb 2" if there is one already (addReturnWithEffect() in Mixer.h).
    auto& song = songOf (sender);
    const auto returns = getReturnTracks (song);
    const auto isTaken = [&song, &returns] (const juce::String& candidate)
    { return std::any_of (returns.begin(), returns.end(), [&] (TrackID other) { return getTrackName (song, other) == candidate; }); };

    auto name = effect->getName();

    for (auto number = 2; isTaken (name); ++number)
        name = effect->getName() + " " + juce::String (number);

    auto* created = addReturnTrack (sender.edit, name, nullptr);

    if (created == nullptr || insertDevice (*created, effect) == nullptr)
        return created;

    if (hasMix (song, getDeviceID (*effect)))
        setMix (song, getDeviceID (*effect), 1.0f);

    addSend (song, getTrackID (sender), getTrackID (*created), false);
    return created;
}

juce::Array<te::AuxSendPlugin*> getSends (const te::Track& track)
{
    return mirror::getSends (track);
}

te::AuxSendPlugin* addSend (te::Track& track, int bus, bool preFader)
{
    auto* target = mirror::findReturnTrack (track.edit, bus);
    return target != nullptr
               ? findDevice<te::AuxSendPlugin> (track.edit, addSend (songOf (track), getTrackID (track), getTrackID (*target), preFader))
               : nullptr;
}

bool isPreFader (const te::AuxSendPlugin& send)
{
    return isPreFader (songOf (send), getDeviceID (send));
}

void setPreFader (te::AuxSendPlugin& send, bool preFader)
{
    setPreFader (songOf (send), getDeviceID (send), preFader);
}

void setSendLevel (te::AuxSendPlugin& send, float decibels)
{
    setSendLevel (songOf (send), getDeviceID (send), decibels);
}

float getSendLevel (const te::AuxSendPlugin& send)
{
    return getSendLevel (songOf (send), getDeviceID (send));
}

te::AuxSendPlugin* addSendTo (te::Track& track, int bus)
{
    auto* target = mirror::findReturnTrack (track.edit, bus);
    return target != nullptr
               ? findDevice<te::AuxSendPlugin> (track.edit, addSendTo (songOf (track), getTrackID (track), getTrackID (*target)))
               : nullptr;
}

te::AuxSendPlugin* addSendToNewReturn (te::Track& track)
{
    return findDevice<te::AuxSendPlugin> (track.edit, addSendToNewReturn (songOf (track), getTrackID (track)));
}

void removeSend (te::AuxSendPlugin& send)
{
    removeSend (songOf (send), getDeviceID (send));
}

//==============================================================================
te::RackInstance* findContainerOf (const te::Plugin& device)
{
    if (findDeviceState (device.edit, getDeviceID (device)).isValid())
        return findDevice<te::RackInstance> (device.edit, findContainerOf (songOf (device), getDeviceID (device)));

    const auto rack = device.isInRack() ? device.getOwnerRackType() : nullptr;
    return rack != nullptr ? findContainerOf (*rack) : nullptr;
}

juce::Array<te::Plugin*> getContainerDevices (const te::Plugin& container)
{
    if (findDeviceState (container.edit, getDeviceID (container)).isValid())
        return findDevices (container.edit, getContainerDevices (songOf (container), getDeviceID (container)));

    auto* rack = getContainerRack (container);
    return rack != nullptr ? getRackDevices (*rack) : juce::Array<te::Plugin*>();
}

juce::ReferenceCountedArray<te::MacroParameter> getContainerMacros (const te::Plugin& container)
{
    auto* rack = getContainerRack (container);
    return rack != nullptr ? rack->getMacroParameters() : juce::ReferenceCountedArray<te::MacroParameter>();
}

te::Track* getTrackOfContainer (const te::RackInstance& container)
{
    if (auto* track = container.getOwnerTrack())
        return track;

    auto& edit = container.edit;
    return edit.getMasterPluginList().contains (&container) ? edit.getMasterTrack() : nullptr;
}

te::Track* getTrackOfContainerDevice (const te::Plugin& device)
{
    auto* container = findContainerOf (device);
    return container != nullptr ? getTrackOfContainer (*container) : nullptr;
}

bool canGroupIntoContainer (const juce::Array<te::Plugin*>& effects)
{
    return ! effects.isEmpty() && effects.getFirst() != nullptr
           && canGroupIntoContainer (songOf (*effects.getFirst()), getDeviceIDs (effects));
}

te::RackInstance* groupIntoContainer (const juce::Array<te::Plugin*>& effects)
{
    if (! canGroupIntoContainer (effects))
        return nullptr;

    auto& edit = effects.getFirst()->edit;
    return findDevice<te::RackInstance> (edit, groupIntoContainer (getSong (edit), getDeviceIDs (effects)));
}

void ungroupContainer (te::RackInstance& container)
{
    ungroupContainer (songOf (container), getDeviceID (container));
}

void setContainerName (te::Plugin& container, const juce::String& name)
{
    setContainerName (songOf (container), getDeviceID (container), name);
}

te::Plugin* insertIntoContainer (te::RackInstance& container, const te::Plugin::Ptr& device, int index)
{
    // Into its rack type, where the song's element of it goes among the container's, which is made from it.
    auto* rack = container.type.get();

    if (rack == nullptr || device == nullptr || isInstrument (*device) || isContainer (*device)
        || ! isContainer (songOf (container), getDeviceID (container)) || ! rack->addPlugin (device, { 0.5f, 0.5f }, false))
        return nullptr;

    insertIntoContainer (songOf (container), getDeviceID (container), makeDeviceState (*device), index);
    return device.get();
}

void moveInContainer (te::Plugin& device, int newIndex)
{
    moveInContainer (songOf (device), getDeviceID (device), newIndex);
}

juce::String makeContainerName (const te::Edit& edit)
{
    return makeContainerName (getSong (edit));
}

//==============================================================================
juce::Array<te::Track*> getTracksFedBy (te::Track& track)
{
    return mirror::findTracks (track.edit, getTracksFedBy (songOf (track), getTrackID (track)));
}

bool hasSidechain (te::Plugin& device)
{
    return hasSidechain (songOf (device), getDeviceID (device));
}

juce::Array<te::AudioTrack*> getSidechainSources (te::Plugin& device)
{
    juce::Array<te::AudioTrack*> sources;

    for (const auto track : getSidechainSources (songOf (device), getDeviceID (device)))
        if (auto* audio = find<te::AudioTrack> (device.edit, track))
            sources.add (audio);

    return sources;
}

te::AudioTrack* getSidechainSource (te::Plugin& device)
{
    return find<te::AudioTrack> (device.edit, getSidechainSource (songOf (device), getDeviceID (device)));
}

SidechainChoice describeSidechainSource (te::Plugin& device)
{
    return describeSidechainSource (songOf (device), getDeviceID (device));
}

std::vector<SidechainChoice> describeTracksToListenTo (te::Edit& edit, te::Track* listener)
{
    return describeTracksToListenTo (getSong (edit), idOf (listener));
}

std::vector<SidechainChoice> describeSidechainChoices (te::Plugin& device)
{
    return describeSidechainChoices (songOf (device), getDeviceID (device));
}

void setSidechainSource (te::Plugin& device, te::AudioTrack* source)
{
    setSidechainSource (songOf (device), getDeviceID (device), idOf (source));
}

te::Plugin* duckUnder (te::AudioTrack& ducked, te::AudioTrack& key)
{
    return mirror::findDevice (ducked.edit, duckUnder (songOf (ducked), getTrackID (ducked), getTrackID (key)));
}
} // namespace phazite
