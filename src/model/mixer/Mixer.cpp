// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Mixer.h"
#include "internal/Containers.h"
#include "internal/Sidechain.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/DevicesArea.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/IDsHeld.h"
#include "model/base/internal/SavedValues.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/TracksValue.h"
#include "model/base/internal/WholeState.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"
#include "model/midi/internal/MidiLinks.h"
#include "model/modulation/internal/Modulation.h"
#include "model/plugins/internal/DeviceStore.h"
#include "model/plugins/internal/PluginHost.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/versions/internal/AreaClashes.h"
#include "model/versions/internal/SongMerge.h"
#include "model/versions/internal/SongPieces.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
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

    std::atomic<std::uint64_t> deviceLooks { 0 }; // countDeviceLooks(): a count alone, which orders nothing

    // The description of one of Phazite's own devices a value is; nullptr for none, a plug-in, a container or a chain.
    const devices::DeviceInfo* findOwnInfo (const DeviceValue* value)
    {
        return value != nullptr && value->form == DeviceValue::Form::own ? findOwnDevice (std::string_view (value->type.toRawUTF8()))
                                                                         : nullptr;
    }

    // A device or a container (not a chain) by its ID, nullptr for none.
    const DeviceValue* findDeviceOrContainer (const Song& song, DeviceID id)
    {
        const auto* value = findDevice (song, id);
        return value != nullptr && value->form != DeviceValue::Form::chain ? value : nullptr;
    }

    // The list of a channel's devices and containers, none for a channel without any (getDeviceChannelPlace()).
    const immer::flex_vector<DeviceID>* findChannelList (const Song& song, TrackID channel)
    {
        return getDevicesValue (song).findList (getDeviceChannelPlace (song, channel));
    }

    // A device's, a container's or a chain's element in a song's tree by its ID (the first of it: each track's, a folder's before
    // the tracks in it, then the master's), invalid for none: what a song that holds no values reads (the loader's), whose
    // tree holds its devices.
    juce::ValueTree findDeviceElementIn (const juce::ValueTree& state, DeviceID id)
    {
        const auto isIt = [id] (const juce::ValueTree& element)
        { return readDeviceForm (element).has_value() && readDeviceElementID (element) == id; };

        const auto inChannel = [&isIt] (const juce::ValueTree& channel) -> juce::ValueTree
        {
            for (const auto& child : channel)
            {
                if (! (child.hasType (schema::DEVICE) || child.hasType (schema::CONTAINER)))
                    continue;

                if (isIt (child))
                    return child;

                for (const auto& chain : child)
                {
                    if (! chain.hasType (schema::CHAIN))
                        continue;

                    if (isIt (chain))
                        return chain;

                    for (const auto& device : chain)
                        if (device.hasType (schema::DEVICE) && isIt (device))
                            return device;
                }
            }

            return {};
        };

        const std::function<juce::ValueTree (const juce::ValueTree&)> inTracks = [&inChannel, &inTracks] (const juce::ValueTree& list)
        {
            for (const auto& track : list)
            {
                if (! track.hasType (schema::TRACK))
                    continue;

                if (auto found = inChannel (track); found.isValid())
                    return found;

                if (auto found = inTracks (track); found.isValid())
                    return found;
            }

            return juce::ValueTree();
        };

        if (auto found = inTracks (state.getChildWithName (schema::TRACKS)); found.isValid())
            return found;

        return inChannel (state.getChildWithName (schema::MASTER));
    }

    // What a container holds (its chains) or a chain (its devices), in their order; none for anything else.
    const immer::flex_vector<DeviceID>* findInside (const SongDevices& devices, const DeviceValue& value)
    {
        if (value.form == DeviceValue::Form::container)
            return devices.chains.find (value.id.getRaw());

        return value.form == DeviceValue::Form::chain ? devices.chainDevices.find (value.id.getRaw()) : nullptr;
    }

    juce::String getOwnName (const devices::DeviceInfo& info)
    {
        return juce::String (info.name.data(), info.name.size());
    }

    // Takes a device out of the song (in the caller's undo step), with what its channel's devices hold of their states
    // stored first, so that an undo brings it back as it was.
    void removeDeviceState (Song& song, DeviceID id)
    {
        // Found once, and taken out at its place (no walk again).
        if (const auto place = findDevicePlace (song, id))
        {
            song.storeDeviceStates ((place->onMaster ? getMasterID (song) : place->track).getRaw());
            eraseDevice (song, id, *place);
            forgetWhatNamesTheGone (song);
        }
    }

    // What a maker made given the place it is put at: the first's, the channel of the rest (a container's chains and their
    // devices, a chain's devices) with the container and the chain they are in, and a container's macros on its channel.
    void placeMade (MadeDevices& made, const DevicePlace& place)
    {
        if (made.devices.empty())
            return;

        auto& first = made.devices.front();
        first.place = place;

        for (size_t i = 1; i < made.devices.size(); ++i)
        {
            auto& inside = made.devices[i];
            inside.place.track = place.track;
            inside.place.onMaster = place.onMaster;

            if (first.form == DeviceValue::Form::chain)
            {
                inside.place.container = place.container;
                inside.place.chain = first.id;
            }
            else
            {
                inside.place.container = first.id;
            }
        }

        for (auto& macro : made.macros)
            macro.place = { .track = place.track, .onMaster = place.onMaster, .container = first.id };
    }

    // A made container's macros, set through modulation's setter of the whole value in the caller's step, once the container
    // is in the song (Modulation.h: setAllModulation(), which finds no owner by walking the song, as setModulator() would
    // for each).
    void setMadeMacros (Song& song, const std::vector<ModulatorValue>& macros)
    {
        auto* values = song.getValues();

        if (macros.empty() || values == nullptr)
            return;

        auto next = values->modulation.get();

        for (const auto& macro : macros)
            next = withHeldModulator (next, macro);

        setAllModulation (song, std::move (next));
    }

    // The song's sends as its area holds them (SongValues.h: sends), a pointer into the area's value as it is now, valid until
    // its next change; none for a song that holds no values (the loader's song, a check's copy), whose tree holds them.
    const SongSends* findHeldSends (const Song& song)
    {
        countSendLook();
        const auto* values = song.getValues();
        return values != nullptr ? &values->sends.get() : nullptr;
    }

    // A value of a channel's as the song holds it, to a ten-thousandth (a volume comes back from a fader's position a little
    // off), the one it has without where it is that near it.
    float roundChannelValue (float value, float none)
    {
        if (const auto rounded = std::round ((double) value * 1.0e4) / 1.0e4; std::abs (rounded - (double) none) >= 1.0e-4)
            return (float) rounded;

        return none;
    }

    // One of a channel's values (its volume, its pan) set, the tracks' narrow setter that a drag's events write through: what
    // the song holds of the channel (the tracks' keeper's: no element read) given the value, and where it is another, that one
    // property of its element written as the song holds it (TracksValue.h: writeTrackProperty(), the rule of the tracks'
    // setters), in the mode of the moment.
    void setChannelField (Song& song, TrackID channel, float TrackValue::* field, const juce::Identifier& property, float value)
    {
        auto next = findHeldTrack (song, channel.isValid() ? channel : getMasterID (song));

        if (! next.has_value() || juce::exactlyEqual ((*next).*field, value))
            return;

        (*next).*field = value;

        if (auto state = findChainState (song, channel); state.isValid())
        {
            auto* undo = song.isWritingOutsideHistory() ? nullptr : &song.getUndoManager();

            if (const auto written = writeTrackProperty (*next, property); written.isVoid())
                state.removeProperty (property, undo);
            else
                state.setProperty (property, written, undo);
        }
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

const SongDevices& getDevicesValue (const Song& song)
{
    if (const auto* values = song.getValues())
        return values->devices.get();

    // A song that holds no values (the loader's, a check's copy): its tree read whole, kept for the thread where it differs
    // from what was read before, so that what was found in it stays while the tree doesn't change.
    thread_local SongDevices fromTree;

    if (auto read = readDevicesValue (song.getState()); ! (read == fromTree))
        fromTree = std::move (read);

    return fromTree;
}

const DeviceValue* findDevice (const Song& song, DeviceID id)
{
    if (! id.isValid())
        return nullptr;

    deviceLooks.fetch_add (1, std::memory_order_relaxed);

    if (const auto* values = song.getValues())
        return values->devices.get().find (id);

    // A song that holds no values (the loader's, the tests' check of a song made of a tree): the one element of the ID read
    // from its tree (never every device of the song for each look), kept for the thread among the last 16 read, so that what
    // reads one, and another, still holds the first: good through the next 15 such looks on the thread.
    const auto element = findDeviceElementIn (song.getState(), id);
    auto value = element.isValid() ? readDeviceValue (element, readDevicePlace (element)) : std::nullopt;

    if (! value.has_value())
        return nullptr;

    thread_local std::deque<DeviceValue> readFromTree;
    readFromTree.push_back (std::move (*value));

    if (readFromTree.size() > 16)
        readFromTree.pop_front();

    return &readFromTree.back();
}

std::uint64_t countDeviceLooks() noexcept
{
    return deviceLooks.load (std::memory_order_relaxed);
}

bool hasDevice (const Song& song, DeviceID id)
{
    return findDevice (song, id) != nullptr;
}

bool isPlugin (const Song& song, DeviceID id)
{
    const auto* value = findDevice (song, id);
    return value != nullptr && value->isPlugin();
}

bool isInstrument (const Song& song, DeviceID id)
{
    const auto* value = findDeviceOrContainer (song, id);
    return value != nullptr && value->isInstrument();
}

std::vector<DeviceID> getDevices (const Song& song, TrackID channel)
{
    const auto* list = findChannelList (song, channel);
    return list != nullptr ? std::vector<DeviceID> (list->begin(), list->end()) : std::vector<DeviceID>();
}

DeviceID getInstrument (const Song& song, TrackID track)
{
    const auto* list = findChannelList (song, track);

    if (list == nullptr || list->empty())
        return {};

    const auto* first = getDevicesValue (song).find (list->front());
    return first != nullptr && first->isInstrument() ? first->id : DeviceID();
}

std::vector<DeviceID> getEffects (const Song& song, TrackID channel)
{
    auto devices = getDevices (song, channel);
    std::erase (devices, getInstrument (song, channel));
    return devices;
}

bool hasInstrument (const Song& song, TrackID track)
{
    const auto* list = findChannelList (song, track);
    const auto& devices = getDevicesValue (song);

    return list != nullptr
           && std::any_of (list->begin(), list->end(),
                           [&devices] (DeviceID id)
                           {
                               const auto* value = devices.find (id);
                               return value != nullptr && value->isInstrument();
                           });
}

TrackID getDeviceChannel (const Song& song, DeviceID device)
{
    // The channel of its place, for a device in a container too: a track's ID, or the master's.
    const auto* value = findDeviceOrContainer (song, device);

    if (value == nullptr)
        return {};

    return value->place.onMaster ? getMasterID (song) : value->place.track;
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
    const auto* value = findDeviceOrContainer (song, id);
    return value != nullptr ? getDeviceName (*value) : juce::String();
}

juce::String getDeviceName (const juce::ValueTree& deviceInCopy)
{
    switch (readDeviceForm (deviceInCopy).value_or (DeviceValue::Form::chain))
    {
        case DeviceValue::Form::container:
            return readDeviceElementName (deviceInCopy);
        case DeviceValue::Form::plugin:
            return readPluginIdentity (deviceInCopy).name;
        case DeviceValue::Form::own:
            if (const auto* own = findOwnDevice (std::string_view (readDeviceType (deviceInCopy).toRawUTF8())))
                return getOwnName (*own);

            return {};
        case DeviceValue::Form::chain:
            break;
    }

    return {};
}

bool opensWindow (const Song& song, DeviceID id)
{
    return isPlugin (song, id);
}

bool popsOut (const Song& song, DeviceID id)
{
    return findOwnInfo (findDevice (song, id)) != nullptr;
}

//==============================================================================
namespace
{
    // An outline's entry of what a value is.
    DeviceOutline outlineOf (const DeviceValue& value, TrackID channel)
    {
        DeviceOutline outline;
        outline.id = value.id;
        outline.form = value.form;
        outline.channel = channel;
        outline.container = value.place.container;
        outline.chain = value.place.chain;

        if (value.form == DeviceValue::Form::own)
            outline.type = value.type;

        if (value.form == DeviceValue::Form::plugin)
            outline.plugin = value.plugin;

        outline.instrument = value.isInstrument();
        outline.sidechain = value.sidechain;
        return outline;
    }

    // Each item of a channel's list in the outline's order (a container followed by its chains, each chain by its devices),
    // after what is there already.
    void forEachOnChannel (const SongDevices& devices, const DevicePlace& channel, const std::function<void (const DeviceValue&)>& each)
    {
        const auto* list = devices.findList (channel);

        if (list == nullptr)
            return;

        const std::function<void (DeviceID)> visit = [&devices, &each, &visit] (DeviceID id)
        {
            const auto* value = devices.find (id);

            if (value == nullptr)
                return;

            each (*value);

            if (const auto* inside = findInside (devices, *value))
                for (const auto item : *inside)
                    visit (item);
        };

        for (const auto id : *list)
            visit (id);
    }

    void addOutlineOf (const Song& song, const SongDevices& devices, TrackID channel, std::vector<DeviceOutline>& outline)
    {
        const auto place = getDeviceChannelPlace (song, channel);
        const auto named = place.onMaster ? getMasterID (song) : channel;
        forEachOnChannel (devices, place, [&outline, named] (const DeviceValue& value) { outline.push_back (outlineOf (value, named)); });
    }

    // Each sound file a device's value names, after those the list holds already.
    void addSoundFilesOf (const DeviceValue& device, std::vector<DeviceSoundFile>& files)
    {
        if (device.form != DeviceValue::Form::own)
            return;

        const auto& properties = getSoundPropertiesOf (std::string_view (device.type.toRawUTF8()));

        for (const auto& [index, path] : device.sounds)
            if (index < properties.size() && schema::isSoundFileProperty (properties[index]) && path.isNotEmpty())
                files.push_back ({ .device = device.id, .property = properties[index], .path = path });
    }
} // namespace

std::vector<DeviceOutline> getDeviceOutline (const Song& song, TrackID channel)
{
    std::vector<DeviceOutline> outline;
    addOutlineOf (song, getDevicesValue (song), channel, outline);
    return outline;
}

std::vector<DeviceOutline> getSongDeviceOutline (const Song& song)
{
    std::vector<DeviceOutline> outline;
    // Read once (a copy); for a song that holds no values (the loader's) its tree's fields alone, never a device's values.
    const auto devices = song.getValues() != nullptr ? getDevicesValue (song) : readDevicesFields (song.getState());

    for (const auto& track : getTrackStates (song))
        addOutlineOf (song, devices, getTrackID (track), outline);

    addOutlineOf (song, devices, {}, outline);
    return outline;
}

const devices::DeviceInfo* getOwnDeviceInfo (const Song& song, DeviceID id)
{
    return findOwnInfo (findDevice (song, id));
}

bool holdsPlugin (const Song& song, TrackID track)
{
    const auto outline = getDeviceOutline (song, track);
    return std::any_of (outline.begin(), outline.end(), [] (const DeviceOutline& each) { return each.form == DeviceValue::Form::plugin; });
}

std::optional<DevicePlace> findContainerPlace (const Song& song, DeviceID container)
{
    const auto* value = findDevice (song, container);

    if (value == nullptr || value->form != DeviceValue::Form::container)
        return std::nullopt;

    return value->place;
}

std::vector<DeviceSoundFile> getDeviceSoundFiles (const Song& song, TrackID channel)
{
    // In the outline's order: a channel's devices, and its containers' chains' devices in their places.
    std::vector<DeviceSoundFile> files;
    forEachOnChannel (getDevicesValue (song), getDeviceChannelPlace (song, channel),
                      [&files] (const DeviceValue& value) { addSoundFilesOf (value, files); });
    return files;
}

std::vector<DeviceSoundFile> getDeviceSoundFiles (const Song& song)
{
    // Every channel's in the song's order: each track's (a folder's before the tracks in it), then the master's.
    std::vector<DeviceSoundFile> files;
    const auto& devices = getDevicesValue (song);
    const auto addOf = [&files, &devices] (const DevicePlace& channel)
    { forEachOnChannel (devices, channel, [&files] (const DeviceValue& value) { addSoundFilesOf (value, files); }); };

    for (const auto& track : getTrackStates (song))
        addOf (makeChannelPlace (getTrackID (track), false));

    addOf (makeChannelPlace ({}, true));
    return files;
}

juce::ValueTree copyWholeDevice (const Song& song, DeviceID id)
{
    // Its element with what it holds, made whole: its store's, its window's place, a container's macros.
    if (findDeviceOrContainer (song, id) == nullptr)
        return {};

    auto element = writeDeviceWhole (getDevicesValue (song), id);
    makeWhole (element, keepValues (song));
    return element;
}

//==============================================================================
std::optional<DevicePlace> findDevicePlace (const Song& song, DeviceID id)
{
    const auto* value = findDeviceOrContainer (song, id);
    return value != nullptr ? std::optional (value->place) : std::nullopt;
}

std::optional<DevicePlace> findChainPlace (const Song& song, DeviceID chain)
{
    const auto* value = findDevice (song, chain);
    return value != nullptr && value->form == DeviceValue::Form::chain ? std::optional (value->place) : std::nullopt;
}

std::vector<DeviceValue> getChannelDeviceValues (const Song& song, TrackID channel)
{
    // Each with what it holds, in the outline's order.
    std::vector<DeviceValue> values;
    forEachOnChannel (getDevicesValue (song), getDeviceChannelPlace (song, channel),
                      [&values] (const DeviceValue& value) { values.push_back (value); });
    return values;
}

juce::String getDeviceName (const DeviceValue& value)
{
    switch (value.form)
    {
        case DeviceValue::Form::own:
            if (const auto* own = findOwnDevice (std::string_view (value.type.toRawUTF8())))
                return getOwnName (*own);

            return {};

        case DeviceValue::Form::plugin:
            return value.plugin.name;
        case DeviceValue::Form::container:
            return value.name;
        case DeviceValue::Form::chain:
            break;
    }

    return {};
}

DevicePlace getDeviceChannelPlace (const Song& song, TrackID channel)
{
    if (findChainState (song, channel).hasType (schema::MASTER))
        return { .onMaster = true };

    return { .track = channel };
}

MadeDevices makeOwnDevice (Song& song, std::string_view typeID, const std::vector<std::pair<juce::String, float>>& values)
{
    DeviceValue device;
    device.id = DeviceID::fromRaw (song.makeID());
    device.form = DeviceValue::Form::own;
    device.type = juce::String (typeID.data(), typeID.size());

    // Its values as the song holds them: none at its default, each within its range and on its steps (SavedValues.h).
    if (const auto* own = findOwnDevice (typeID))
    {
        for (const auto& [parameter, value] : values)
        {
            if (const auto index = devices::findParameter (*own, parameter.toStdString()); index >= 0)
            {
                if (const auto held = keepHeldValue (own->parameters[(size_t) index], value))
                    device.values = device.values.set ((std::uint32_t) index, *held);
                else
                    device.values = device.values.erase ((std::uint32_t) index);
            }
        }
    }

    MadeDevices made;
    made.devices.push_back (std::move (device));
    return made;
}

MadeDevices makeDevice (Song& song, const DeviceType& type)
{
    if (type.isBuiltIn)
        return makeOwnDevice (song, type.description.fileOrIdentifier.toStdString());

    DeviceValue plugin;
    plugin.id = DeviceID::fromRaw (song.makeID());
    plugin.form = DeviceValue::Form::plugin;
    plugin.plugin = fromDescription (type.description);

    MadeDevices made;
    made.devices.push_back (std::move (plugin));
    return made;
}

PluginIdentity fromDescription (const juce::PluginDescription& description)
{
    return { .format = description.pluginFormatName,
             .name = description.name,
             .maker = description.manufacturerName,
             .identifier = description.fileOrIdentifier,
             .uid = description.uniqueId,
             .instrument = description.isInstrument };
}

DeviceID insertDevice (Song& song, TrackID channel, MadeDevices made, int index)
{
    auto chain = findChainState (song, channel);

    if (made.devices.empty() || ! chain.isValid() || made.devices.front().form == DeviceValue::Form::chain)
        return {};

    const auto id = made.devices.front().id;
    const auto instrument = made.devices.front().isInstrument();

    if (instrument && ! canHoldInstrument (chain))
        return {};

    // A plain folder mixes its tracks through its devices as a bus.
    if (isFolderState (chain) && ! isBus (song, channel))
        makeBus (song, channel);

    placeMade (made, getDeviceChannelPlace (song, channel));

    if (! setNewDevices (song, made, instrument ? 0 : index)) // instruments come first
        return {};

    setMadeMacros (song, made.macros);
    return id;
}

DeviceID setInstrument (Song& song, TrackID track, MadeDevices instrument)
{
    if (instrument.devices.empty() || ! instrument.devices.front().isInstrument() || ! canHoldInstrument (findTrackState (song, track)))
        return {};

    if (const auto old = getInstrument (song, track); old.isValid())
        removeDeviceState (song, old);

    return insertDevice (song, track, std::move (instrument), 0);
}

void copyDevices (Song& song, CopiedIDs& copied)
{
    // The tracks copied whose copies hold no device, each original's devices read once from its element, gathered before any
    // device's ID joins the copied.
    std::vector<std::pair<TrackID, TrackID>> channels;

    for (const auto& [original, copy] : copied)
    {
        const auto from = TrackID::fromRaw (original), to = TrackID::fromRaw (copy);

        if (findTrackState (song, from).hasType (schema::TRACK) && findTrackState (song, to).hasType (schema::TRACK)
            && ! getDevices (song, from).empty() && getDevices (song, to).empty())
            channels.emplace_back (from, to);
    }

    for (const auto& [from, to] : channels)
    {
        const auto values = getChannelDeviceValues (song, from);

        // Each by the ID the copied give it (a duplicate's: Schema.h, giveNewIDs()), else a new one of the song's, given it.
        const auto copyOf = [&song, &copied] (DeviceID id)
        {
            if (const auto found = copied.find (id.getRaw()); found != copied.end())
                return DeviceID::fromRaw (found->second);

            const auto fresh = DeviceID::fromRaw (song.makeID());
            copied[id.getRaw()] = fresh.getRaw();
            return fresh;
        };

        for (const auto& value : values)
            copyOf (value.id);

        // Each device or container of the channel, with what it holds, one element put after the others, with what the store
        // holds of each (the song's values alone), under the copies' IDs.
        const auto store = copyStoreOf (song, copied);
        MadeDevices made;

        const auto put = [&song, &store, &made, to]
        {
            if (made.devices.empty())
                return;

            made.store = store;
            placeMade (made, getDeviceChannelPlace (song, to));
            setNewDevices (song, made);
            made = {};
        };

        for (const auto& value : values)
        {
            auto copy = value;
            copy.id = copyOf (value.id);
            copy.place.container = value.place.container.isValid() ? copyOf (value.place.container) : DeviceID();
            copy.place.chain = value.place.chain.isValid() ? copyOf (value.place.chain) : DeviceID();

            // A sidechain that listens to a track copied too listens to the copy, as a copy names what it holds.
            if (const auto source = copied.find (value.sidechain.getRaw()); value.sidechain.isValid() && source != copied.end())
                copy.sidechain = TrackID::fromRaw (source->second);

            if (! value.place.container.isValid())
                put();

            made.devices.push_back (std::move (copy));
        }

        put();
    }

    // Their windows' places, view state outside the history, as a copy of their elements carried them.
    copyWindowPlaces (song, copied);
}

bool eraseChannelDevices (Song& song, const std::vector<TrackID>& channels)
{
    auto* values = song.getValues();

    if (values == nullptr)
        return false;

    // Each channel's list alone, each item with what it holds.
    auto next = values->devices.get();

    for (const auto channel : channels)
        if (const auto* list = next.findList (getDeviceChannelPlace (song, channel)))
            for (const auto id : immer::flex_vector<DeviceID> (*list))
                next = withoutDevice (next, id);

    return values->setAllDevices (song, std::move (next));
}

void takeDevicesOf (Song& song, const juce::ValueTree& channel)
{
    auto* values = song.getValues();
    const auto onMaster = channel.hasType (schema::MASTER);

    if (values == nullptr || ! (onMaster || channel.hasType (schema::TRACK)))
        return;

    const auto place = makeChannelPlace (getTrackID (channel), onMaster);

    if (! onMaster && ! hasTrack (song, place.track))
        return;

    // Each as the song holds it, an owner before what it holds, one of an ID the song holds left out with what it holds.
    auto next = values->devices.get();

    const auto add = [&next] (const juce::ValueTree& element, const DevicePlace& at)
    {
        auto value = readHeldDevice (element, at);

        if (! value.has_value() || next.find (value->id) != nullptr)
            return DeviceID();

        const auto id = value->id;
        next = withDevice (next, std::move (*value));
        return id;
    };

    for (const auto& child : channel)
    {
        if (! (child.hasType (schema::DEVICE) || child.hasType (schema::CONTAINER)))
            continue;

        if (const auto container = add (child, place); container.isValid() && child.hasType (schema::CONTAINER))
            for (const auto& chain : child)
                if (const auto id = chain.hasType (schema::CHAIN) ? add (chain, makeContainerPlace (place, container)) : DeviceID();
                    id.isValid())
                    for (const auto& device : chain)
                        if (device.hasType (schema::DEVICE))
                            add (device, makeContainerPlace (place, container, id));
    }

    values->setAllDevices (song, std::move (next));
}

void removeDevice (Song& song, DeviceID id)
{
    if (findDeviceOrContainer (song, id) == nullptr)
        return;

    const UndoStep step (song, "Remove " + getDeviceName (song, id));
    const auto channel = getDeviceChannel (song, id);

    // A container goes with what it holds, and the devices after a device in a container take its place.
    removeDeviceState (song, id);

    if (const auto folder = findTrackState (song, channel);
        isFolderState (folder) && isBus (song, channel) && getDevices (song, channel).empty() && ! (bool) folder[schema::folderBus])
        makePlainFolder (song, channel);
}

std::set<std::uint64_t> getItemIDs (const Song& song)
{
    // The tree's items (Schema.h: holdsID()), as what names one names it, and those the song's values hold (IDsHeld.h:
    // forEachNameableIDHeld(): a modulator's and a macro's once they leave the tree, and a send's).
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

void forgetWhatNamesTheGone (Song& song)
{
    // A container's macros, with what they modulate, where the song holds the container no more (removed, ungrouped): erased
    // through modulation's code, in one set of its area, before the IDs the song holds are counted, so that what names a
    // macro (a MIDI link of its value) goes below as what names any item gone.
    if (auto* values = song.getValues())
    {
        const auto& devices = values->devices.get();
        std::vector<ModulatorID> macros;

        for (const auto& [container, list] : values->modulation.get().containers)
            if (const auto* held = devices.find (DeviceID::fromRaw (container));
                held == nullptr || held->form != DeviceValue::Form::container)
                macros.insert (macros.end(), list.begin(), list.end());

        if (! macros.empty())
            values->eraseModulators (song, macros);
    }

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

    // An automation curve of a parameter of what is gone, a send's among them, through automation's code, which looks each
    // curve's owner up among the IDs above and reads no curve; the step's Undo brings them back with the rest.
    forgetCurvesOfTheGone (song, ids);

    // A pad's track whose drum sampler is gone: deleted, or, holding clips, a plain track.
    std::vector<TrackID> padTracks;

    for (const auto& pad : getPadTracks (song))
    {
        if (ids.contains (pad.sampler.getRaw()))
            continue;

        if (! hasClips (song, pad.track))
        {
            padTracks.push_back (pad.track);
            continue;
        }

        setTrackFields (song, pad.track,
                        [] (TrackValue& value)
                        {
                            value.padDevice = {};
                            value.pad.reset();
                        });
    }

    if (! padTracks.empty())
        deleteTracks (song, padTracks, nullptr); // which forgets what named them in turn
}

void moveDevice (Song& song, DeviceID id, int newIndex)
{
    if (findDeviceOrContainer (song, id) == nullptr)
        return;

    // To the place of the device that is there now: after it when moving down, before it when moving up.
    const UndoStep step (song, "Move Device");
    moveInDeviceList (song, id, newIndex);
}

void setDeviceOn (Song& song, DeviceID id, bool on)
{
    if (findDeviceOrContainer (song, id) == nullptr || isDeviceOn (song, id) == on)
        return;

    const UndoStep step (song, on ? "Turn Device On" : "Turn Device Off");
    setDeviceFields (song, id, [on] (DeviceValue& value) { value.off = ! on; });
}

bool isDeviceOn (const Song& song, DeviceID id)
{
    const auto* value = findDeviceOrContainer (song, id);
    return value == nullptr || ! value->off;
}

bool hasMix (const Song& song, DeviceID id)
{
    const auto* value = findDeviceOrContainer (song, id);

    if (const auto* own = findOwnInfo (value))
        return own->kind != devices::DeviceKind::instrument && devices::findParameter (*own, devices::mixParameterID) >= 0;

    return value != nullptr && (value->form == DeviceValue::Form::container || value->isPlugin()) && ! value->isInstrument();
}

float getMix (const Song& song, DeviceID id)
{
    if (! hasMix (song, id))
        return 1.0f;

    const auto* value = findDeviceOrContainer (song, id);

    if (const auto* own = findOwnInfo (value))
        return value->getParameterValue (devices::findParameter (*own, devices::mixParameterID));

    return value->mix;
}

float getDefaultMix (const Song& song, DeviceID id)
{
    if (const auto* own = hasMix (song, id) ? findOwnInfo (findDevice (song, id)) : nullptr)
        return own->parameters[(size_t) devices::findParameter (*own, devices::mixParameterID)].defaultValue;

    return 1.0f;
}

void setMix (Song& song, DeviceID id, float mix)
{
    if (! hasMix (song, id))
        return;

    // One of Phazite's own effects' mix parameter, or a plug-in's or a container's mix.
    const auto wet = juce::jlimit (0.0f, 1.0f, mix);

    if (const auto* own = findOwnInfo (findDevice (song, id)))
        setDeviceValue (song, id, devices::findParameter (*own, devices::mixParameterID), wet);
    else
        setDeviceFields (song, id, [wet] (DeviceValue& value) { value.mix = wet; });
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
    return placeDevice (song, track, makeDevice (song, type), selection);
}

DeviceID placeDevice (Song& song, TrackID track, MadeDevices device, Selection* selection)
{
    if (device.devices.empty() || device.devices.front().form == DeviceValue::Form::chain)
        return {};

    if (! device.devices.front().isInstrument())
        return insertDevice (song, track, std::move (device));

    // An instrument would silence a track's audio clips, so a track with audio gets one next to it instead, and so does a
    // folder, a return or none.
    auto target = track;

    if (! canHoldInstrument (findTrackState (song, track)) || holdsAudioClips (song, track))
    {
        target = insertTrack (song, findTrackState (song, track).hasType (schema::TRACK) ? track : TrackID(), selection);

        if (! target.isValid())
            return {};

        const auto name = getDeviceName (device.devices.front());
        setTrackName (song, target, name);

        if (const auto colour = getColourForName (name))
            setTrackColour (song, target, *colour);
    }

    return setInstrument (song, target, std::move (device));
}

DeviceID chooseInstrument (Song& song, TrackID track, const DeviceType& type)
{
    const UndoStep step (song, (getInstrument (song, track).isValid() ? "Replace Instrument with " : "Add ") + type.name);
    return setInstrument (song, track, makeDevice (song, type));
}

DeviceID addEffect (Song& song, TrackID channel, const DeviceType& type)
{
    const UndoStep step (song, "Add " + type.name);
    return insertDevice (song, channel, makeDevice (song, type));
}

TrackID addReturnWith (Song& song, TrackID sender, const DeviceType& type)
{
    const UndoStep step (song, "Add " + type.name + " Return");
    return addReturnWithEffect (song, sender, makeDevice (song, type));
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
        setTrackFields (song, folder, [] (TrackValue& value) { value.bus = false; });
        makePlainFolder (song, folder);
    }
}

DeviceID getFader (const Song& song, TrackID channel)
{
    return readVolumeDevice (findChainState (song, channel));
}

juce::String getVolumeParameterID (const Song& song, TrackID channel)
{
    const auto state = findChainState (song, channel);

    if (state.hasType (schema::MASTER))
        return schema::masterVolume;

    return readChannelVCA (state).isValid() ? schema::vcaVolume : schema::faderVolume;
}

juce::String getPanParameterID (const Song& song, TrackID channel)
{
    const auto state = findChainState (song, channel);

    if (state.hasType (schema::MASTER))
        return schema::masterPan;

    return readChannelVCA (state).isValid() ? juce::String() : juce::String (schema::faderPan);
}

ChannelRef findChannelOfFader (const Song& song, DeviceID fader)
{
    if (! fader.isValid())
        return {};

    for (const auto& channel : getTrackStates (song))
        if (readChannelFader (channel) == fader || readChannelVCA (channel) == fader)
            return { .track = readTrackElementID (channel) };

    if (const auto master = findChainState (song, {}); readChannelFader (master) == fader || readChannelVCA (master) == fader)
        return { .onMaster = true };

    return {};
}

float keepVolumeInRange (float decibels)
{
    return std::isfinite (decibels) ? juce::jlimit (lowestVolume, highestVolume, decibels) : 0.0f;
}

void setChannelVolume (Song& song, TrackID channel, float decibels)
{
    const auto none =
        TrackValue::defaultVolume (channel.isValid() && ! isMaster (song, channel) ? TrackValue::Kind::track : TrackValue::Kind::master);
    setChannelField (song, channel, &TrackValue::volume, schema::channelVolume, roundChannelValue (keepVolumeInRange (decibels), none));
}

void setChannelPan (Song& song, TrackID channel, float pan)
{
    if (getPanParameterID (song, channel).isNotEmpty())
        setChannelField (song, channel, &TrackValue::pan, schema::channelPan,
                         roundChannelValue (std::isfinite (pan) ? juce::jlimit (-1.0f, 1.0f, pan) : 0.0f, 0.0f));
}

float getChannelVolume (const Song& song, TrackID channel)
{
    return readChannelVolume (findChainState (song, channel));
}

float getChannelPan (const Song& song, TrackID channel)
{
    return readChannelPan (findChainState (song, channel));
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

TrackID addReturnWithEffect (Song& song, TrackID sender, MadeDevices effect)
{
    if (effect.devices.empty() || effect.devices.front().form == DeviceValue::Form::chain || effect.devices.front().isInstrument())
        return {};

    // Named after the effect: "Reverb", or "Reverb 2" if there is one already.
    const auto returns = getReturnTracks (song);
    const auto isTaken = [&song, &returns] (const juce::String& candidate)
    { return std::any_of (returns.begin(), returns.end(), [&] (TrackID other) { return getTrackName (song, other) == candidate; }); };

    const auto& made = effect.devices.front();
    const auto effectName = made.form == DeviceValue::Form::own ? getDeviceName (made) : made.plugin.name;
    auto name = effectName;

    for (auto number = 2; isTaken (name); ++number)
        name = effectName + " " + juce::String (number);

    const auto created = addReturnTrack (song, name, nullptr);

    if (! created.isValid())
        return {};

    const auto device = insertDevice (song, created, std::move (effect));

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

bool eraseTrackSends (Song& song, const std::vector<TrackID>& tracks)
{
    auto* values = song.getValues();

    if (values == nullptr)
        return false;

    std::vector<DeviceID> gone;

    for (const auto track : tracks)
        if (const auto* list = values->sends.get().tracks.find (track.getRaw()))
            gone.insert (gone.end(), list->begin(), list->end());

    return ! gone.empty() && values->eraseSends (song, gone);
}

void takeSendsOf (Song& song, const juce::ValueTree& track)
{
    auto* values = song.getValues();
    const auto from = getTrackID (track);

    if (values == nullptr || ! track.hasType (schema::TRACK) || ! from.isValid())
        return;

    for (const auto& element : track)
        if (element.hasType (schema::SEND))
            if (auto value = readHeldSend (element, from); value.has_value() && values->sends.get().find (value->id) == nullptr)
                values->setSend (song, std::move (*value));
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
