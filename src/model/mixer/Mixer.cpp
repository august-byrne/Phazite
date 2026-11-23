// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Mixer.h"
#include "internal/Containers.h"
#include "internal/Sidechain.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/IDsHeld.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"
#include "model/midi/internal/MidiLinks.h"
#include "model/modulation/internal/Modulation.h"
#include "model/plugins/internal/PluginHost.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/versions/internal/AreaClashes.h"
#include "model/versions/internal/SongMerge.h"
#include "model/versions/internal/SongPieces.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    constexpr float newSendDb = -6.0f;

    std::atomic<std::uint64_t> deviceWalks { 0 }; // countDeviceWalks(): a count alone, which orders nothing

    juce::UndoManager* getUndo (Song& song)
    {
        return &song.getUndoManager();
    }

    bool isDeviceElement (const juce::ValueTree& state)
    {
        return state.hasType (schema::DEVICE) || state.hasType (schema::CONTAINER);
    }

    // The description of one of Phazite's own devices an element names; nullptr for a plug-in or a container. Its type read
    // through the devices' conversion (DevicesValue.h), as every reader below reads one field of one device.
    const devices::DeviceInfo* findOwnInfo (const juce::ValueTree& deviceState)
    {
        return readDeviceForm (deviceState) == DeviceValue::Form::own ? findOwnDevice (readDeviceType (deviceState).toStdString())
                                                                      : nullptr;
    }

    // A device's, a container's or a chain's element by its ID, invalid for one the song hasn't: what findDevice() reads
    // while the song's tree holds its devices.
    juce::ValueTree findDeviceElement (const Song& song, DeviceID id)
    {
        if (auto device = findDeviceState (song, id); device.isValid())
            return device;

        return findContainerChain (song, id);
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
            forgetWhatNamesTheGone (song);
        }
    }

    // The IDs of the items that tracks, devices, chains and sends are (Schema.h: holdsID()), as what names one names it: the
    // tree's (its clips and its automation aside, which hold none that these name), and those the song's values hold of
    // items that something can name (IDsHeld.h: forEachNameableIDHeld(), a modulator's and a macro's once they leave the
    // tree, which a MIDI link names, and a send's, which a curve, a MIDI link and a modulation name).
    std::set<std::uint64_t> getItemIDs (const Song& song)
    {
        std::set<std::uint64_t> ids;
        forEachNameableIDHeld (song, [&ids] (std::uint64_t id) { ids.insert (id); });
        const std::function<void (const juce::ValueTree&)> visit = [&ids, &visit] (const juce::ValueTree& tree)
        {
            for (const auto& child : tree)
            {
                if (child.hasType (schema::CLIP) || child.hasType (schema::AUTOMATION))
                    continue;

                for (const auto* property : { &schema::trackID, &schema::channelFader, &schema::channelVCA })
                    if (child.hasProperty (*property) && schema::holdsID (child, *property))
                        ids.insert (readID (child[*property]));

                visit (child);
            }
        };

        visit (song.getState());
        return ids;
    }

    // The song's sends as its area holds them (SongValues.h: sends), a pointer into the area's value as it is now, valid until
    // its next change; none for a song that holds no values (the loader's song, a check's copy), whose tree holds them.
    const SongSends* findHeldSends (const Song& song)
    {
        countSendLook();
        const auto* values = song.getValues();
        return values != nullptr ? &values->sends.get() : nullptr;
    }

    // Sets a property without an undo step, only if its value is another (as a store of a plug-in's state does:
    // model/plugins/internal/PluginInstances.h). JUCE workaround W8 (docs/juce-workarounds.md).
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
        return hasClips (song, track, ClipValue::Kind::audio);
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

    // A container's are its chains', in their order.
    if (isContainerState (chain))
    {
        for (const auto& child : chain)
            if (child.hasType (schema::CHAIN))
                for (const auto& device : child)
                    if (device.hasType (schema::DEVICE))
                        devices.push_back (device);

        return devices;
    }

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

    countDeviceWalk();

    for (const auto& track : getTrackStates (song))
        if (const auto found = findIn (track); found.isValid())
            return found;

    return findIn (song.getState().getChildWithName (schema::MASTER));
}

DeviceID getDeviceID (const juce::ValueTree& deviceState)
{
    return readDeviceElementID (deviceState);
}

std::uint64_t countDeviceWalks() noexcept
{
    return deviceWalks.load (std::memory_order_relaxed);
}

void countDeviceWalk() noexcept
{
    deviceWalks.fetch_add (1, std::memory_order_relaxed);
}

bool isPluginState (const juce::ValueTree& deviceState)
{
    return readDeviceForm (deviceState) == DeviceValue::Form::plugin;
}

bool holdsPlugin (const juce::ValueTree& tree)
{
    if (isPluginState (tree))
        return true;

    if (tree.hasType (schema::DEVICE) && ! isContainerState (tree))
        return false;

    for (const auto& child : tree)
        if ((child.hasType (schema::DEVICE) || child.hasType (schema::CONTAINER) || child.hasType (schema::CHAIN)) && holdsPlugin (child))
            return true;

    return false;
}

bool isContainerState (const juce::ValueTree& deviceState)
{
    return deviceState.hasType (schema::CONTAINER);
}

bool isInstrumentState (const juce::ValueTree& deviceState)
{
    return readIsInstrument (deviceState);
}

SongDevices getDevicesValue (const Song& song)
{
    return readDevicesValue (song.getState());
}

std::optional<DeviceValue> findDevice (const Song& song, DeviceID id)
{
    const auto element = findDeviceElement (song, id);
    return element.isValid() ? readDeviceValue (element, readDevicePlace (element)) : std::nullopt;
}

bool hasDevice (const Song& song, DeviceID id)
{
    return id.isValid() && findDeviceElement (song, id).isValid();
}

bool isPlugin (const Song& song, DeviceID id)
{
    return isPluginState (findDeviceState (song, id));
}

bool isInstrument (const Song& song, DeviceID id)
{
    return readIsInstrument (findDeviceState (song, id));
}

std::vector<DeviceID> getDevices (const Song& song, TrackID channel)
{
    return readDeviceList (findChainState (song, channel));
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
    // Out of its chain and its container, for a device in one.
    auto parent = findDeviceState (song, device).getParent();

    while (parent.hasType (schema::CHAIN) || isContainerState (parent))
        parent = parent.getParent();

    return getTrackID (parent);
}

bool needsInstrument (const Song& song, TrackID track)
{
    const auto state = findTrackState (song, track);

    if (! canHoldInstrument (state) || getInstrument (song, track).isValid())
        return false;

    return hasClips (song, track, ClipValue::Kind::midi);
}

juce::String getDeviceName (const Song& song, DeviceID id)
{
    return getDeviceName (findDeviceState (song, id));
}

juce::String getDeviceName (const juce::ValueTree& state)
{
    if (isContainerState (state))
        return readDeviceElementName (state);

    if (isPluginState (state))
        return readPluginIdentity (state).name;

    if (const auto* own = findOwnInfo (state))
        return getOwnName (*own);

    return {};
}

bool opensWindow (const Song& song, DeviceID id)
{
    return isPlugin (song, id);
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

    // Its values, but for those at their defaults, which the song keeps none of (keepValueSetByHand()).
    if (const auto* own = findOwnInfo (state))
        for (const auto& [parameter, value] : values)
            if (const auto index = devices::findParameter (*own, parameter.toStdString()); index >= 0)
                keepValueSetByHand (state, own->parameters[(size_t) index], value, nullptr);

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

void forgetWhatNamesTheGone (Song& song)
{
    auto* undo = getUndo (song);
    auto ids = getItemIDs (song);

    // A send to a return that is gone, with the ID it is known by: the sends' next value worked out whole, then set once.
    if (auto* values = song.getValues())
    {
        const auto held = values->sends.get();
        auto next = held;

        for (const auto& [key, send] : held.sends)
        {
            if (ids.contains (send.to.getRaw()))
                continue;

            ids.erase (key);
            next = withoutSend (next, send.id);
        }

        values->setAllSends (song, std::move (next));
    }

    // A modulation of a parameter of what is gone, a send's among them, through modulation's code.
    forgetModulationsOf (song, [&ids] (DeviceID device) { return ! ids.contains (device.getRaw()); });

    // A MIDI link of a parameter of what is gone.
    forgetMidiLinksOfTheGone (song, ids);

    // A pad's track whose drum sampler is gone: deleted, or, holding clips, a plain track.
    std::vector<TrackID> padTracks;

    for (auto state : getTrackStates (song))
    {
        if (! state.hasProperty (schema::trackPad) || ids.contains (readID (state[schema::trackPadDevice])))
            continue;

        if (! hasClips (song, getTrackID (state)))
        {
            padTracks.push_back (getTrackID (state));
            continue;
        }

        state.removeProperty (schema::trackPadDevice, undo);
        state.removeProperty (schema::trackPad, undo);
    }

    if (! padTracks.empty())
        deleteTracks (song, padTracks, nullptr); // which forgets what named them in turn
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
    return ! readDeviceOff (findDeviceState (song, id));
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

    return (isContainerState (state) || isPluginState (state)) && ! isInstrumentState (state);
}

float getMix (const Song& song, DeviceID id)
{
    if (! hasMix (song, id))
        return 1.0f;

    const auto state = findDeviceState (song, id);

    if (const auto* own = findOwnInfo (state))
    {
        const auto& mix = own->parameters[(size_t) devices::findParameter (*own, devices::mixParameterID)];
        return readDeviceParameterValue (state, mix);
    }

    return readDeviceMix (state);
}

float getDefaultMix (const Song& song, DeviceID id)
{
    if (const auto* own = hasMix (song, id) ? findOwnInfo (findDeviceState (song, id)) : nullptr)
        return own->parameters[(size_t) devices::findParameter (*own, devices::mixParameterID)].defaultValue;

    return 1.0f;
}

void setMix (Song& song, DeviceID id, float mix)
{
    if (! hasMix (song, id))
        return;

    auto state = findDeviceState (song, id);
    const auto wet = juce::jlimit (0.0f, 1.0f, mix);

    if (const auto* own = findOwnInfo (state))
        keepValueSetByHand (state, own->parameters[(size_t) devices::findParameter (*own, devices::mixParameterID)], wet, getUndo (song));
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
    return toDescription (readPluginIdentity (device));
}

juce::PluginDescription toDescription (const PluginIdentity& identity)
{
    juce::PluginDescription description;
    description.pluginFormatName = identity.format;
    description.name = identity.name;
    description.manufacturerName = identity.maker;
    description.fileOrIdentifier = identity.identifier;
    description.uniqueId = identity.uid;
    description.isInstrument = identity.instrument;
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

        const auto name = getDeviceName (device);
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

float keepVolumeInRange (float decibels)
{
    return std::isfinite (decibels) ? juce::jlimit (lowestVolume, highestVolume, decibels) : 0.0f;
}

void setChannelVolume (Song& song, TrackID channel, float decibels)
{
    if (auto state = findChainState (song, channel); state.isValid())
        setChannelValue (state, schema::channelVolume, keepVolumeInRange (decibels),
                         state.hasType (schema::MASTER) ? defaultMasterVolume : 0.0f, getUndo (song));
}

void setChannelPan (Song& song, TrackID channel, float pan)
{
    if (auto state = findChainState (song, channel); state.isValid() && getPanParameterID (song, channel).isNotEmpty())
        setChannelValue (state, schema::channelPan, std::isfinite (pan) ? juce::jlimit (-1.0f, 1.0f, pan) : 0.0f, 0.0f, getUndo (song));
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
    const auto effectName = own != nullptr ? getOwnName (*own) : readPluginIdentity (effect).name;
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
    if (const auto* held = findHeldSends (song))
    {
        const auto* list = held->tracks.find (track.getRaw());
        return list != nullptr ? std::vector<DeviceID> (list->begin(), list->end()) : std::vector<DeviceID>();
    }

    std::vector<DeviceID> sends;

    for (const auto& send : getSendValues (song, track))
        sends.push_back (send.id);

    return sends;
}

std::optional<SendValue> getSendValue (const Song& song, DeviceID send)
{
    if (! send.isValid())
        return std::nullopt;

    if (const auto* held = findHeldSends (song))
    {
        const auto* found = held->find (send);
        return found != nullptr ? std::optional<SendValue> (*found) : std::nullopt;
    }

    for (const auto& track : getTrackStates (song))
        if (auto found = findTrackSend (track, send))
            return found;

    return std::nullopt;
}

TrackID getSendTrack (const Song& song, DeviceID send)
{
    const auto found = getSendValue (song, send);
    return found.has_value() ? found->from : TrackID();
}

std::vector<SendValue> getSendValues (const Song& song, TrackID track)
{
    if (const auto* held = findHeldSends (song))
    {
        std::vector<SendValue> sends;

        if (const auto* list = held->tracks.find (track.getRaw()))
            for (const auto id : *list)
                if (const auto* send = held->find (id))
                    sends.push_back (*send);

        return sends;
    }

    return readTrackSends (findTrackState (song, track));
}

DeviceID findSendFromTo (const Song& song, TrackID from, TrackID returnTrack)
{
    if (const auto* held = findHeldSends (song))
    {
        if (const auto* list = held->tracks.find (from.getRaw()))
            for (const auto id : *list)
                if (const auto* send = held->find (id); send != nullptr && send->to == returnTrack)
                    return id;

        return {};
    }

    for (const auto& send : getSendValues (song, from))
        if (send.to == returnTrack)
            return send.id;

    return {};
}

TrackID getSendTarget (const Song& song, DeviceID send)
{
    const auto found = getSendValue (song, send);
    return found.has_value() ? found->to : TrackID();
}

DeviceID addSend (Song& song, TrackID from, TrackID returnTrack, bool preFader)
{
    // The song's, after the track's other sends, to the return.
    auto state = findTrackState (song, from);

    if (! state.hasType (schema::TRACK) || isFolderState (state) || ! isReturnTrack (song, returnTrack) || from == returnTrack)
        return {};

    // Nor to a return whose sound comes back to the track through sends and buses: a loop, which Phazite's engine can't play,
    // whatever offered the send (a knob shown before the routing changed). One that comes back only through a sidechain is
    // made, and the sidechain goes (forgetSidechainLoops(), below), as FL Studio's routing does.
    {
        std::vector<TrackID> reached { returnTrack };

        for (size_t i = 0; i < reached.size(); ++i)
        {
            std::vector<TrackID> next;

            for (const auto& send : getSendValues (song, reached[i]))
                next.push_back (send.to);

            for (auto parent = getParentFolder (song, reached[i]); parent.isValid(); parent = getParentFolder (song, parent))
                if (isBus (song, parent))
                    next.push_back (parent);

            for (const auto track : next)
                if (std::find (reached.begin(), reached.end(), track) == reached.end())
                    reached.push_back (track);
        }

        if (std::find (reached.begin(), reached.end(), from) != reached.end())
            return {};
    }

    auto* values = song.getValues();

    if (values == nullptr)
        return {};

    // After the track's other sends, in the step in progress.
    const auto id = DeviceID::fromRaw (song.makeID());
    values->setSend (song, { .id = id, .from = from, .to = returnTrack, .level = newSendDb, .preFader = preFader });
    forgetSidechainLoops (song); // a sidechain on the track that listens to the return, say
    return id;
}

bool isPreFader (const Song& song, DeviceID send)
{
    const auto found = getSendValue (song, send);
    return found.has_value() && found->preFader;
}

void setPreFader (Song& song, DeviceID send, bool preFader)
{
    auto* values = song.getValues();
    auto found = getSendValue (song, send);

    if (values == nullptr || ! found.has_value() || found->preFader == preFader)
        return;

    const UndoStep step (song, "Change Send");
    found->preFader = preFader;
    values->setSend (song, *found);
}

void setSendLevel (Song& song, DeviceID send, float decibels)
{
    auto* values = song.getValues();

    if (auto found = getSendValue (song, send); values != nullptr && found.has_value())
    {
        found->level = keepSendLevel (decibels);
        values->setSend (song, *found); // nothing for the level it holds
    }
}

float getSendLevel (const Song& song, DeviceID send)
{
    const auto found = getSendValue (song, send);
    return found.has_value() ? found->level : 0.0f;
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
    if (auto* values = song.getValues(); values != nullptr && getSendValue (song, send).has_value())
    {
        const UndoStep step (song, "Remove Send");
        values->eraseSend (song, send);
        forgetWhatNamesTheGone (song);
    }
}

void copySends (Song& song, CopiedIDs& copied)
{
    auto* values = song.getValues();

    if (values == nullptr)
        return;

    // The tracks copied whose copies hold none, by the sends' lists alone: each original's, onto its copy, gathered before
    // any send's ID joins the copied.
    auto next = values->sends.get();
    std::vector<std::pair<TrackID, std::vector<SendValue>>> tracks;

    for (const auto& [original, copy] : copied)
    {
        if (const auto* list = next.tracks.find (original); list != nullptr && next.tracks.find (copy) == nullptr)
        {
            std::vector<SendValue> sends;

            for (const auto id : *list)
                if (const auto* send = next.find (id))
                    sends.push_back (*send);

            tracks.emplace_back (TrackID::fromRaw (copy), std::move (sends));
        }
    }

    // Each a new ID, in the copied IDs, before what names one (a modulation, a curve) is copied by them; its return the
    // copy's where the return was copied too, as a copy names what it holds (Schema.h: giveNewIDs()).
    for (const auto& [track, sends] : tracks)
    {
        for (auto copy : sends)
        {
            const auto original = copy.id;
            copy.id = DeviceID::fromRaw (song.makeID());
            copy.from = track;

            if (const auto to = copied.find (copy.to.getRaw()); to != copied.end())
                copy.to = TrackID::fromRaw (to->second);

            copied[original.getRaw()] = copy.id.getRaw();
            next = withSend (next, std::move (copy));
        }
    }

    values->setAllSends (song, std::move (next));
}

AreaChoice chooseSendClash (Song& song, const MergeClash& clash, int value, const MergeSides& sides)
{
    // A path through a SEND ("TRACKS:", "TRACK:12", "SEND:44"): sends merge as items by their IDs, each property of theirs at
    // a time (SongMerge.cpp); so a clash is a property of one, or one whole (added or taken out on a side).
    int step = -1;

    for (int i = 0; i < clash.path.size() && step < 0; ++i)
        if (clash.path[i].startsWith (schema::SEND.toString() + ":"))
            step = i;

    if (step < 0)
        return AreaChoice::notTheArea;

    // Refused (nothing written) for what a merge of two songs Phazite holds never makes, but a commit's song may come from
    // anyone (tests/FuzzTests.cpp chooses every clash of a damaged merge): a path past the send, one whose track the song
    // hasn't (as the tree's choice found no place for it), a send the song holds on another track, its ID, a property that
    // isn't a send's, an element of another type or ID, one that stands in for what its version couldn't read where it would
    // be read (one chosen whole, or put back: SongPieces.h, holdsStandIn()), as modulation's and the curves' choosers refuse
    // one, and a send the song would read otherwise than it was chosen (SendsValue.h: readHeldSend(): a level finer than a
    // ten-thousandth).
    auto* values = song.getValues();

    if (step == 0 || clash.path.size() != step + 1 || ! juce::isPositiveAndBelow (value, 3) || values == nullptr)
        return AreaChoice::refused;

    auto ownerPath = clash.path;
    ownerPath.removeRange (step, clash.path.size() - step);
    const auto owner = findByPath (song.getState(), ownerPath);
    const auto from = getTrackID (owner);
    const auto id = DeviceID::fromRaw (readID (clash.path[step].fromFirstOccurrenceOf (":", false, false)));
    const auto held = getSendValue (song, id);

    if (! owner.hasType (schema::TRACK) || ! from.isValid() || ! id.isValid() || (held.has_value() && held->from != from))
        return AreaChoice::refused;

    const juce::ValueTree* elements[] = { &clash.baseElement, &clash.oursElement, &clash.theirsElement };
    const juce::var* chosenValues[] = { &clash.base, &clash.ours, &clash.theirs };

    // Its own property: the value chosen given to it (SendsValue.h: setSendProperty(), a void value taking it away as the tree
    // did). A send both lines made under one ID (only in a damaged commit's song: the merge renumbers what both lines add),
    // whose property's clash the side chosen holds no send for, taken away rather than left without the property (known bug
    // 239's shape), where that side's song can be read; and one the song holds none of (an earlier choice took it away) put
    // back first as the side chosen holds it, as modulation's chooser puts back a modulation. Where the song holds the send,
    // the side's element is asked only whether it is there (one that stands in is), never read, as modulation's chooser asks
    // of a modulation: the value chosen goes to the send the song holds.
    if (clash.property.isValid())
    {
        if (clash.property == schema::sendID)
            return AreaChoice::refused;

        const juce::ValueTree* songs[] = { &sides.base, &sides.before, &sides.theirs };
        const auto& side = *songs[value];
        const auto canRead = side.hasType (schema::PHAZITE);
        const auto there = canRead ? findByPath (side, clash.path) : juce::ValueTree();

        if (held.has_value() && canRead && ! there.isValid())
        {
            values->eraseSend (song, id);
            return AreaChoice::chosen;
        }

        // One the song holds none of (an earlier choice took it away) put back first as the side chosen holds it.
        const auto start = held.has_value() ? held : there.isValid() && ! holdsStandIn (there) ? readHeldSend (there, from) : std::nullopt;

        if (! start.has_value() || start->id != id)
            return AreaChoice::refused;

        auto next = *start;

        if (const auto read =
                setSendProperty (next, clash.property, *chosenValues[value]) ? readHeldSend (writeSendElement (next), from) : std::nullopt;
            ! read.has_value() || ! (*read == next))
            return AreaChoice::refused;

        values->setSend (song, next);
        return AreaChoice::chosen;
    }

    // One whole: the side chosen's (read as the song holds one) made the song's, in its place or after the track's others, or
    // the song's taken out where that side had none.
    const auto& chosen = *elements[value];

    if (! chosen.isValid())
    {
        values->eraseSend (song, id);
        return AreaChoice::chosen;
    }

    const auto read = readHeldSendID (chosen) == id && ! holdsStandIn (chosen) ? readHeldSend (chosen, from) : std::nullopt;

    if (! read.has_value())
        return AreaChoice::refused;

    values->setSend (song, *read);
    return AreaChoice::chosen;
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

    // By its format's own ID first (JUCE's number for it, which for a VST3 is made from its class ID), with its maker,
    // wherever this computer keeps its file and whatever it is called now; then by its file and name; then by name.
    if (wanted.uniqueId != 0)
        for (const auto& type : types)
            if (type.pluginFormatName == wanted.pluginFormatName && type.uniqueId == wanted.uniqueId
                && type.manufacturerName == wanted.manufacturerName)
                return type;

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
    if (! isPlugin (song, id))
        return ids;

    if (auto* instance = findPluginInstance (song, id))
        for (const auto& parameter : instance->getParameterInfos())
            if (ids.size() < maximum && parameter.isAutomatable && parameter.id != getMixID())
                ids.addIfNotAlreadyThere (parameter.id);

    return ids;
}
} // namespace phazite
