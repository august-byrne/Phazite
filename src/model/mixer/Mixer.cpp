// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Mixer.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Devices.h"
#include "model/plugins/internal/PluginHost.h"
#include "model/plugins/internal/PluginInstances.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace phazite
{
namespace
{
    constexpr float newSendDb = -6.0f;

    juce::UndoManager* getUndo (Song& song)
    {
        return &song.getUndoManager();
    }

    bool isDeviceElement (const juce::ValueTree& state)
    {
        return state.hasType (schema::DEVICE) || state.hasType (schema::CONTAINER);
    }

    // The description of one of Phazite's own devices an element names; nullptr for a plug-in or a container.
    const devices::DeviceInfo* findOwnInfo (const juce::ValueTree& deviceState)
    {
        return deviceState.hasType (schema::DEVICE) && ! isPluginState (deviceState)
                   ? findOwnDevice (deviceState[schema::deviceType].toString().toStdString())
                   : nullptr;
    }

    juce::String getOwnName (const devices::DeviceInfo& info)
    {
        return juce::String (info.name.data(), info.name.size());
    }

    // Puts a device's element among a chain's, before the one at a place, or after the last.
    void addDeviceState (Song& song, juce::ValueTree chain, const juce::ValueTree& device, int deviceIndex)
    {
        const auto devices = getDeviceStates (chain);
        auto index = chain.getNumChildren();

        if (juce::isPositiveAndBelow (deviceIndex, (int) devices.size()))
            index = chain.indexOf (devices[(size_t) deviceIndex]);
        else if (! devices.empty())
            index = chain.indexOf (devices.back()) + 1;

        chain.addChild (device, index, getUndo (song));
    }

    // Takes a device out of the song (in the caller's undo step), with what its channel's devices hold of their states
    // stored first, so that an undo brings it back as it was.
    void removeDeviceState (Song& song, DeviceID id)
    {
        if (auto state = findDeviceState (song, id); state.isValid())
        {
            song.storeDeviceStates (getDeviceChannel (song, id).getRaw());
            state.getParent().removeChild (state, getUndo (song));
        }
    }

    // Sets a property without an undo step, only if its value is another (as a store of a plug-in's state does:
    // model/plugins/internal/PluginInstances.h).
    void store (juce::ValueTree& state, const juce::Identifier& property, const juce::var& value)
    {
        if (const auto* stored = state.getPropertyPointer (property); stored == nullptr || *stored != value)
            state.setProperty (property, value, nullptr);
    }

    // A value of a channel's in the song, to a ten-thousandth (a volume comes back from a fader's position a little off),
    // none where it is the one it has without.
    void setChannelValue (juce::ValueTree state, const juce::Identifier& property, float value, float none, juce::UndoManager* undo)
    {
        if (const auto rounded = std::round ((double) value * 1.0e4) / 1.0e4; std::abs (rounded - (double) none) < 1.0e-4)
            state.removeProperty (property, undo);
        else
            state.setProperty (property, rounded, undo);
    }

    bool holdsAudioClips (const Song& song, TrackID track)
    {
        const auto clips = getClipStates (findTrackState (song, track));
        return std::any_of (clips.begin(), clips.end(), isAudioState);
    }

    // Whether a track can hold an instrument: a track of the playlist's that is neither a folder nor a return.
    bool canHoldInstrument (const juce::ValueTree& trackState)
    {
        return trackState.hasType (schema::TRACK) && ! isFolderState (trackState) && ! isReturnState (trackState);
    }

    // One of Phazite's own device types, described as a plug-in is: in Phazite's own format, by its type ID.
    DeviceType makeOwnType (const devices::DeviceInfo& info)
    {
        const auto instrument = info.kind == devices::DeviceKind::instrument;
        juce::PluginDescription description;
        description.name = getOwnName (info);
        description.pluginFormatName = schema::ownDeviceFormat;
        description.category = instrument ? "Synth" : "Effect";
        description.manufacturerName = "Phazite";
        description.fileOrIdentifier = juce::String (info.typeID.data(), info.typeID.size());
        description.isInstrument = instrument;
        return makeDeviceType (description);
    }

    // Phazite's own instruments, or its effects.
    void addOwnDevices (std::vector<DeviceType>& types, devices::DeviceKind kind)
    {
        for (const auto* info : getOwnDevices())
            if (info->kind == kind)
                types.push_back (makeOwnType (*info));
    }

    juce::String getMixID()
    {
        return juce::String (devices::mixParameterID.data(), devices::mixParameterID.size());
    }
} // namespace

//==============================================================================
juce::ValueTree findChainState (const Song& song, TrackID channel)
{
    return channel.isValid() ? findTrackState (song, channel) : song.getState().getChildWithName (schema::MASTER);
}

std::vector<juce::ValueTree> getDeviceStates (const juce::ValueTree& chain)
{
    std::vector<juce::ValueTree> devices;

    for (const auto& child : chain)
        if (isDeviceElement (child))
            devices.push_back (child);

    return devices;
}

juce::ValueTree findDeviceState (const Song& song, DeviceID id)
{
    // In a chain, or in one of its containers.
    const auto findIn = [id] (const juce::ValueTree& chain)
    {
        for (const auto& state : getDeviceStates (chain))
        {
            if (getDeviceID (state) == id)
                return state;

            for (const auto& inside : getDeviceStates (state))
                if (getDeviceID (inside) == id)
                    return inside;
        }

        return juce::ValueTree();
    };

    if (! id.isValid())
        return {};

    for (const auto& track : getTrackStates (song))
        if (const auto found = findIn (track); found.isValid())
            return found;

    return findIn (song.getState().getChildWithName (schema::MASTER));
}

DeviceID getDeviceID (const juce::ValueTree& deviceState)
{
    return DeviceID::fromRaw ((std::uint64_t) (juce::int64) deviceState[schema::deviceID]);
}

bool isPluginState (const juce::ValueTree& deviceState)
{
    return deviceState.hasType (schema::DEVICE) && deviceState[schema::deviceType].toString() == schema::deviceIsPlugin;
}

bool isContainerState (const juce::ValueTree& deviceState)
{
    return deviceState.hasType (schema::CONTAINER);
}

bool isInstrumentState (const juce::ValueTree& deviceState)
{
    if (isPluginState (deviceState))
        return deviceState[schema::pluginInstrument];

    const auto* own = findOwnInfo (deviceState);
    return own != nullptr && own->kind == devices::DeviceKind::instrument;
}

std::vector<DeviceID> getDevices (const Song& song, TrackID channel)
{
    std::vector<DeviceID> ids;

    for (const auto& state : getDeviceStates (findChainState (song, channel)))
        ids.push_back (getDeviceID (state));

    return ids;
}

DeviceID getInstrument (const Song& song, TrackID track)
{
    const auto devices = getDeviceStates (findChainState (song, track));
    return ! devices.empty() && isInstrumentState (devices.front()) ? getDeviceID (devices.front()) : DeviceID();
}

std::vector<DeviceID> getEffects (const Song& song, TrackID channel)
{
    auto devices = getDevices (song, channel);
    std::erase (devices, getInstrument (song, channel));
    return devices;
}

bool hasInstrument (const Song& song, TrackID track)
{
    const auto devices = getDeviceStates (findChainState (song, track));
    return std::any_of (devices.begin(), devices.end(), isInstrumentState);
}

TrackID getDeviceChannel (const Song& song, DeviceID device)
{
    auto parent = findDeviceState (song, device).getParent();

    if (isContainerState (parent))
        parent = parent.getParent();

    return getTrackID (parent);
}

bool needsInstrument (const Song& song, TrackID track)
{
    const auto state = findTrackState (song, track);

    if (! canHoldInstrument (state) || getInstrument (song, track).isValid())
        return false;

    const auto clips = getClipStates (state);
    return std::any_of (clips.begin(), clips.end(), isMidiState);
}

juce::String getDeviceName (const Song& song, DeviceID id)
{
    const auto state = findDeviceState (song, id);

    if (isContainerState (state))
        return state[schema::containerName].toString();

    if (isPluginState (state))
        return state[schema::pluginName].toString();

    if (const auto* own = findOwnInfo (state))
        return getOwnName (*own);

    return {};
}

bool opensWindow (const Song& song, DeviceID id)
{
    return isPluginState (findDeviceState (song, id));
}

bool popsOut (const Song& song, DeviceID id)
{
    return findOwnInfo (findDeviceState (song, id)) != nullptr;
}

//==============================================================================
juce::ValueTree makeOwnDeviceState (Song& song, std::string_view typeID, const std::vector<std::pair<juce::String, float>>& values)
{
    juce::ValueTree state (schema::DEVICE);
    state.setProperty (schema::deviceID, (juce::int64) song.makeID(), nullptr);
    state.setProperty (schema::deviceType, juce::String (typeID.data(), typeID.size()), nullptr);

    if (! values.empty())
    {
        juce::ValueTree set (schema::VALUES);

        for (const auto& [parameter, value] : values)
            set.setProperty (juce::Identifier (parameter), value, nullptr);

        state.appendChild (set, nullptr);
    }

    return state;
}

juce::ValueTree makeDeviceState (Song& song, const DeviceType& type)
{
    if (type.isBuiltIn)
        return makeOwnDeviceState (song, type.description.fileOrIdentifier.toStdString());

    juce::ValueTree state (schema::DEVICE);
    state.setProperty (schema::deviceID, (juce::int64) song.makeID(), nullptr);
    state.setProperty (schema::deviceType, schema::deviceIsPlugin, nullptr);
    setPluginIdentity (state, type.description);
    return state;
}

DeviceID insertDeviceState (Song& song, TrackID channel, juce::ValueTree device, int index)
{
    auto chain = findChainState (song, channel);

    if (! chain.isValid() || ! isDeviceElement (device) || (isInstrumentState (device) && ! canHoldInstrument (chain)))
        return {};

    // A plain folder mixes its tracks through its devices as a bus.
    if (isFolderState (chain) && ! isBus (song, channel))
        makeBus (song, channel);

    addDeviceState (song, chain, device, isInstrumentState (device) ? 0 : index); // instruments come first
    return getDeviceID (device);
}

DeviceID setInstrument (Song& song, TrackID track, juce::ValueTree instrument)
{
    if (! isInstrumentState (instrument) || ! canHoldInstrument (findTrackState (song, track)))
        return {};

    if (const auto old = getInstrument (song, track); old.isValid())
        removeDeviceState (song, old);

    return insertDeviceState (song, track, instrument, 0);
}

void removeDevice (Song& song, DeviceID id)
{
    if (! findDeviceState (song, id).isValid())
        return;

    const UndoStep step (song, "Remove " + getDeviceName (song, id));
    const auto channel = getDeviceChannel (song, id);

    // A container goes with what it holds, and the devices after a device in a container take its place.
    removeDeviceState (song, id);

    if (const auto folder = findTrackState (song, channel);
        isFolderState (folder) && isBus (song, channel) && getDevices (song, channel).empty() && ! (bool) folder[schema::folderBus])
        makePlainFolder (song, channel);
}

void moveDevice (Song& song, DeviceID id, int newIndex)
{
    auto state = findDeviceState (song, id);

    if (! state.isValid())
        return;

    const UndoStep step (song, "Move Device");

    // To the place of the device that is there now: after it when moving down, before it when moving up. Clips, lanes
    // and a folder's tracks can sit between devices in the track's element.
    auto chain = state.getParent();
    const auto devices = getDeviceStates (chain);
    const auto& target = devices[(size_t) juce::jlimit (0, (int) devices.size() - 1, newIndex)];
    chain.moveChild (chain.indexOf (state), chain.indexOf (target), getUndo (song));
}

void setDeviceOn (Song& song, DeviceID id, bool on)
{
    auto state = findDeviceState (song, id);

    if (! state.isValid() || isDeviceOn (song, id) == on)
        return;

    const UndoStep step (song, on ? "Turn Device On" : "Turn Device Off");

    if (on)
        state.removeProperty (schema::deviceOff, getUndo (song));
    else
        state.setProperty (schema::deviceOff, true, getUndo (song));
}

bool isDeviceOn (const Song& song, DeviceID id)
{
    return ! (bool) findDeviceState (song, id)[schema::deviceOff];
}

void setDeviceSound (Song& song, DeviceID id, const juce::Identifier& property, const juce::String& value)
{
    if (auto state = findDeviceState (song, id); state.isValid())
    {
        if (value.isNotEmpty())
            state.getOrCreateChildWithName (schema::SOUNDS, getUndo (song)).setProperty (property, value, getUndo (song));
        else if (auto sounds = state.getChildWithName (schema::SOUNDS); sounds.isValid())
            sounds.removeProperty (property, getUndo (song));
    }
}

void rememberWindowPosition (Song& song, DeviceID id, juce::Point<int> topLeft)
{
    if (auto state = findDeviceState (song, id); state.isValid())
    {
        store (state, schema::deviceWindowX, topLeft.x);
        store (state, schema::deviceWindowY, topLeft.y);
    }
}

std::optional<juce::Point<int>> getWindowPosition (const Song& song, DeviceID id)
{
    if (const auto state = findDeviceState (song, id); state.hasProperty (schema::deviceWindowX))
        return juce::Point<int> ((int) state[schema::deviceWindowX], (int) state[schema::deviceWindowY]);

    return std::nullopt;
}

bool hasMix (const Song& song, DeviceID id)
{
    const auto state = findDeviceState (song, id);

    if (const auto* own = findOwnInfo (state))
        return own->kind != devices::DeviceKind::instrument && devices::findParameter (*own, devices::mixParameterID) >= 0;

    return isContainerState (state) || (isPluginState (state) && ! isInstrumentState (state));
}

float getMix (const Song& song, DeviceID id)
{
    if (! hasMix (song, id))
        return 1.0f;

    const auto state = findDeviceState (song, id);

    if (const auto* own = findOwnInfo (state))
    {
        const auto& mix = own->parameters[(size_t) devices::findParameter (*own, devices::mixParameterID)];
        return (float) state.getChildWithName (schema::VALUES).getProperty (juce::Identifier (getMixID()), mix.defaultValue);
    }

    return (float) state.getProperty (schema::deviceMix, 1.0f);
}

void setMix (Song& song, DeviceID id, float mix)
{
    if (! hasMix (song, id))
        return;

    auto state = findDeviceState (song, id);
    const auto wet = juce::jlimit (0.0f, 1.0f, mix);

    if (findOwnInfo (state) != nullptr)
        state.getOrCreateChildWithName (schema::VALUES, getUndo (song)).setProperty (juce::Identifier (getMixID()), wet, getUndo (song));
    else if (wet < 1.0f)
        state.setProperty (schema::deviceMix, wet, getUndo (song));
    else
        state.removeProperty (schema::deviceMix, getUndo (song));
}

void setPluginIdentity (juce::ValueTree& device, const juce::PluginDescription& description)
{
    store (device, schema::pluginFormat, description.pluginFormatName);
    store (device, schema::pluginName, description.name);
    store (device, schema::pluginMaker, description.manufacturerName);
    store (device, schema::pluginIdentifier, description.fileOrIdentifier);
    store (device, schema::pluginUid, description.uniqueId);

    if (description.isInstrument)
        store (device, schema::pluginInstrument, true);
    else if (device.hasProperty (schema::pluginInstrument))
        device.removeProperty (schema::pluginInstrument, nullptr);
}

juce::PluginDescription getPluginIdentity (const juce::ValueTree& device)
{
    juce::PluginDescription description;
    description.pluginFormatName = device[schema::pluginFormat].toString();
    description.name = device[schema::pluginName].toString();
    description.manufacturerName = device[schema::pluginMaker].toString();
    description.fileOrIdentifier = device[schema::pluginIdentifier].toString();
    description.uniqueId = (int) device[schema::pluginUid];
    description.isInstrument = device[schema::pluginInstrument];
    return description;
}

//==============================================================================
DeviceID addDeviceTo (Song& song, TrackID track, const DeviceType& type, Selection* selection)
{
    const UndoStep step (song, "Add " + type.name);
    return placeDevice (song, track, makeDeviceState (song, type), selection);
}

DeviceID placeDevice (Song& song, TrackID track, juce::ValueTree device, Selection* selection)
{
    if (! isDeviceElement (device))
        return {};

    if (! isInstrumentState (device))
        return insertDeviceState (song, track, device);

    // An instrument would silence a track's audio clips, so a track with audio gets one next to it instead, and so does a
    // folder, a return or none.
    auto target = track;

    if (! canHoldInstrument (findTrackState (song, track)) || holdsAudioClips (song, track))
    {
        target = insertTrack (song, findTrackState (song, track).hasType (schema::TRACK) ? track : TrackID(), selection);

        if (! target.isValid())
            return {};

        const auto* own = findOwnInfo (device);
        const auto name = own != nullptr ? getOwnName (*own) : device[schema::pluginName].toString();
        setTrackName (song, target, name);

        if (const auto colour = getColourForName (name))
            setTrackColour (song, target, *colour);
    }

    return setInstrument (song, target, device);
}

DeviceID chooseInstrument (Song& song, TrackID track, const DeviceType& type)
{
    const UndoStep step (song, (getInstrument (song, track).isValid() ? "Replace Instrument with " : "Add ") + type.name);
    return setInstrument (song, track, makeDeviceState (song, type));
}

DeviceID addEffect (Song& song, TrackID channel, const DeviceType& type)
{
    const UndoStep step (song, "Add " + type.name);
    return insertDeviceState (song, channel, makeDeviceState (song, type));
}

TrackID addReturnWith (Song& song, TrackID sender, const DeviceType& type)
{
    const UndoStep step (song, "Add " + type.name + " Return");
    return addReturnWithEffect (song, sender, makeDeviceState (song, type));
}

//==============================================================================
void setMixedAsBus (Song& song, TrackID folder, bool bus)
{
    if (! isFolder (song, folder))
        return;

    const UndoStep step (song, bus ? "Mix Folder as Bus" : "Make Folder Plain");

    if (bus)
    {
        mixAsBus (song, folder);
    }
    else if (getDevices (song, folder).empty())
    {
        findTrackState (song, folder).removeProperty (schema::folderBus, getUndo (song));
        makePlainFolder (song, folder);
    }
}

DeviceID getFader (const Song& song, TrackID channel)
{
    const auto state = findChainState (song, channel);
    const auto& property = state.hasProperty (schema::channelVCA) ? schema::channelVCA : schema::channelFader;
    return DeviceID::fromRaw ((std::uint64_t) (juce::int64) state[property]);
}

juce::String getVolumeParameterID (const Song& song, TrackID channel)
{
    const auto state = findChainState (song, channel);

    if (state.hasType (schema::MASTER))
        return schema::masterVolume;

    return state.hasProperty (schema::channelVCA) ? schema::vcaVolume : schema::faderVolume;
}

juce::String getPanParameterID (const Song& song, TrackID channel)
{
    const auto state = findChainState (song, channel);

    if (state.hasType (schema::MASTER))
        return schema::masterPan;

    return state.hasProperty (schema::channelVCA) ? juce::String() : juce::String (schema::faderPan);
}

juce::ValueTree findChannelOfFader (const Song& song, DeviceID fader)
{
    if (! fader.isValid())
        return {};

    auto channels = getTrackStates (song);
    channels.push_back (song.getState().getChildWithName (schema::MASTER));

    for (const auto& channel : channels)
        for (const auto* property : { &schema::channelFader, &schema::channelVCA })
            if ((std::uint64_t) (juce::int64) channel.getProperty (*property, (juce::int64) 0) == fader.getRaw())
                return channel;

    return {};
}

void setChannelVolume (Song& song, TrackID channel, float decibels)
{
    if (auto state = findChainState (song, channel); state.isValid())
        setChannelValue (state, schema::channelVolume, decibels, state.hasType (schema::MASTER) ? defaultMasterVolume : 0.0f,
                         getUndo (song));
}

void setChannelPan (Song& song, TrackID channel, float pan)
{
    if (auto state = findChainState (song, channel); state.isValid() && getPanParameterID (song, channel).isNotEmpty())
        setChannelValue (state, schema::channelPan, juce::jlimit (-1.0f, 1.0f, pan), 0.0f, getUndo (song));
}

float getChannelVolume (const Song& song, TrackID channel)
{
    const auto state = findChainState (song, channel);
    return (float) state.getProperty (schema::channelVolume, state.hasType (schema::MASTER) ? defaultMasterVolume : 0.0f);
}

float getChannelPan (const Song& song, TrackID channel)
{
    return (float) findChainState (song, channel).getProperty (schema::channelPan, 0.0f);
}

TrackID getOutputBus (const Song& song, TrackID track)
{
    for (auto parent = getParentFolder (song, track); parent.isValid(); parent = getParentFolder (song, parent))
        if (isBus (song, parent))
            return parent;

    return {};
}

std::vector<TrackID> getChannelTracks (const Song& song)
{
    auto tracks = getTrackIDs (song);

    for (const auto returnTrack : getReturnTracks (song))
        if (std::find (tracks.begin(), tracks.end(), returnTrack) == tracks.end())
            tracks.push_back (returnTrack);

    return tracks;
}

//==============================================================================
std::vector<TrackID> getReturnTracks (const Song& song)
{
    std::vector<TrackID> returns;

    for (const auto& state : getTrackStates (song))
        if (isReturnState (state))
            returns.push_back (getTrackID (state));

    return returns;
}

TrackID addReturnTrack (Song& song, const juce::String& name, Selection* selection)
{
    return insertReturnTrack (song, name, selection);
}

TrackID addNewReturnTrack (Song& song, Selection* selection)
{
    return addReturnTrack (song, "Return " + juce::String ((int) getReturnTracks (song).size() + 1), selection);
}

TrackID addReturnWithEffect (Song& song, TrackID sender, juce::ValueTree effect)
{
    if (! isDeviceElement (effect) || isInstrumentState (effect))
        return {};

    // Named after the effect: "Reverb", or "Reverb 2" if there is one already.
    const auto returns = getReturnTracks (song);
    const auto isTaken = [&song, &returns] (const juce::String& candidate)
    { return std::any_of (returns.begin(), returns.end(), [&] (TrackID other) { return getTrackName (song, other) == candidate; }); };

    const auto* own = findOwnInfo (effect);
    const auto effectName = own != nullptr ? getOwnName (*own) : effect[schema::pluginName].toString();
    auto name = effectName;

    for (auto number = 2; isTaken (name); ++number)
        name = effectName + " " + juce::String (number);

    const auto created = addReturnTrack (song, name, nullptr);

    if (! created.isValid())
        return {};

    const auto device = insertDeviceState (song, created, effect);

    // The dry sound reaches the master through the track itself, so a return plays only its effect.
    if (hasMix (song, device))
        setMix (song, device, 1.0f);

    addSend (song, sender, created, false);
    return created;
}

std::vector<DeviceID> getSends (const Song& song, TrackID track)
{
    std::vector<DeviceID> sends;

    for (const auto& child : findTrackState (song, track))
        if (child.hasType (schema::SEND))
            sends.push_back (DeviceID::fromRaw ((std::uint64_t) (juce::int64) child[schema::sendID]));

    return sends;
}

juce::ValueTree findSendState (const Song& song, DeviceID send)
{
    if (send.isValid())
        for (const auto& track : getTrackStates (song))
            for (const auto& child : track)
                if (child.hasType (schema::SEND) && (std::uint64_t) (juce::int64) child[schema::sendID] == send.getRaw())
                    return child;

    return {};
}

TrackID getSendTarget (const Song& song, DeviceID send)
{
    return TrackID::fromRaw ((std::uint64_t) (juce::int64) findSendState (song, send)[schema::sendTo]);
}

DeviceID addSend (Song& song, TrackID from, TrackID returnTrack, bool preFader)
{
    // The song's, after the track's other sends, to the return.
    auto state = findTrackState (song, from);

    if (! state.hasType (schema::TRACK) || isFolderState (state) || ! isReturnTrack (song, returnTrack) || from == returnTrack)
        return {};

    const auto id = song.makeID();
    juce::ValueTree send (schema::SEND);
    send.setProperty (schema::sendID, (juce::int64) id, nullptr);
    send.setProperty (schema::sendTo, (juce::int64) returnTrack.getRaw(), nullptr);
    send.setProperty (schema::sendLevel, newSendDb, nullptr);

    if (preFader)
        send.setProperty (schema::sendPreFader, true, nullptr);

    state.appendChild (send, getUndo (song));
    return DeviceID::fromRaw (id);
}

bool isPreFader (const Song& song, DeviceID send)
{
    return (bool) findSendState (song, send)[schema::sendPreFader];
}

void setPreFader (Song& song, DeviceID send, bool preFader)
{
    auto state = findSendState (song, send);

    if (! state.isValid() || isPreFader (song, send) == preFader)
        return;

    const UndoStep step (song, "Change Send");

    if (preFader)
        state.setProperty (schema::sendPreFader, true, getUndo (song));
    else
        state.removeProperty (schema::sendPreFader, getUndo (song));
}

void setSendLevel (Song& song, DeviceID send, float decibels)
{
    if (auto state = findSendState (song, send); state.isValid())
        setChannelValue (state, schema::sendLevel, decibels, 0.0f, getUndo (song));
}

float getSendLevel (const Song& song, DeviceID send)
{
    return (float) findSendState (song, send).getProperty (schema::sendLevel, 0.0f);
}

DeviceID addSendTo (Song& song, TrackID from, TrackID returnTrack)
{
    const UndoStep step (song, "Add Send");
    return addSend (song, from, returnTrack, false);
}

DeviceID addSendToNewReturn (Song& song, TrackID from)
{
    const UndoStep step (song, "Add Send");
    const auto created = addNewReturnTrack (song, nullptr);
    return created.isValid() ? addSend (song, from, created, false) : DeviceID();
}

void removeSend (Song& song, DeviceID send)
{
    if (auto state = findSendState (song, send); state.isValid())
    {
        const UndoStep step (song, "Remove Send");
        state.getParent().removeChild (state, getUndo (song));
    }
}

//==============================================================================
void addMixerCommands (CommandRegistry& registry, Selection& selection)
{
    registry.add ({ .id = "track.addReturn",
                    .name = "Add Return Track",
                    .category = "Track",
                    .description = "Adds a return track, for an effect such as a reverb that several tracks send to.",
                    .synonyms = { "aux", "bus", "send", "fx return", "reverb" },
                    .perform = withSong (selection, [&selection] (Song& song) { addNewReturnTrack (song, &selection); }),
                    .isEnabled = [&selection] { return selection.getSong() != nullptr; },
                    .undoTarget = undoTargetOf (selection) });
}

//==============================================================================
DeviceType makeDeviceType (const juce::PluginDescription& description)
{
    return { description.name, description.isInstrument ? "Instrument" : "Effect", description, description.isInstrument,
             description.pluginFormatName == schema::ownDeviceFormat };
}

std::vector<DeviceType> getDeviceTypes (const PluginHost& host)
{
    // Phazite's own devices (roadmap step 1.6), instruments first, then the plug-ins found by scanning.
    std::vector<DeviceType> types;
    addOwnDevices (types, devices::DeviceKind::instrument);
    addOwnDevices (types, devices::DeviceKind::effect);

    for (const auto& description : host.getKnownPlugins().getTypes())
        types.push_back (makeDeviceType (description));

    return types;
}

std::optional<juce::PluginDescription> findKnownPlugin (const PluginHost& host, const juce::PluginDescription& wanted)
{
    const auto types = host.getKnownPlugins().getTypes();

    for (const auto& type : types)
        if (wanted.fileOrIdentifier.isNotEmpty() && type.fileOrIdentifier == wanted.fileOrIdentifier && type.name == wanted.name)
            return type;

    for (const auto& type : types)
        if (type.name == wanted.name && type.manufacturerName == wanted.manufacturerName
            && type.pluginFormatName == wanted.pluginFormatName)
            return type;

    return {};
}

juce::PluginDescription getPluginDescription (const PluginHost& host, const juce::ValueTree& device)
{
    const auto description = getPluginIdentity (device);
    return findKnownPlugin (host, description).value_or (description);
}

juce::StringArray getKeyControls (const Song& song, DeviceID id, int maximum)
{
    juce::StringArray ids;

    // A plug-in's own parameters, as its instance has them; Phazite's own devices show theirs in their editors, and a
    // container shows its macros. The ID a device's mix goes by names the mix, not a parameter of the plug-in's.
    if (! isPluginState (findDeviceState (song, id)))
        return ids;

    if (auto* instance = findPluginInstance (song, id))
        for (const auto& parameter : instance->getParameterInfos())
            if (ids.size() < maximum && parameter.isAutomatable && parameter.id != getMixID())
                ids.addIfNotAlreadyThere (parameter.id);

    return ids;
}
} // namespace phazite
