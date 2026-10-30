// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Mixer.h"
#include "internal/Containers.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DeviceFiles.h"
#include "model/devices/internal/DevicePlugin.h"
#include "model/devices/internal/DeviceStates.h"
#include "model/devices/internal/Devices.h"
#include "model/engine/internal/LoudnessMeterPlugin.h"
#include "model/mirror/internal/Mirror.h"

#include <optional>

namespace phazite
{
namespace
{
    constexpr float newSendDb = -6.0f;

    // The dry and wet levels Tracktion gives a plug-in or a container, each passing its sound at full level at 1.
    struct MixLevels
    {
        te::AutomatableParameter::Ptr dry, wet;
    };

    std::optional<MixLevels> findMixLevels (te::Plugin& device)
    {
        if (isInstrument (device))
            return {};

        auto dry = device.getAutomatableParameterByID ("dry level");
        auto wet = device.getAutomatableParameterByID ("wet level");

        if (dry == nullptr || wet == nullptr)
            return {};

        return MixLevels { dry, wet };
    }

    bool isMixLevel (const te::AutomatableParameter& parameter)
    {
        return parameter.paramID == "dry level" || parameter.paramID == "wet level";
    }

    // The mix of one of Phazite's own effects: a parameter of its own, which its plug-in applies.
    te::AutomatableParameter::Ptr findOwnMix (te::Plugin& device)
    {
        if (auto* own = dynamic_cast<DevicePlugin*> (&device); own != nullptr && ! own->isSynth())
            return own->findParameter (devices::mixParameterID);

        return {};
    }

    template <typename... Types>
    bool isAnyOf (const te::Plugin& plugin)
    {
        return (... || (dynamic_cast<const Types*> (&plugin) != nullptr));
    }

    // The plug-ins that are part of a channel rather than devices on it.
    bool isChannelPlugin (const te::Plugin& plugin)
    {
        return isAnyOf<te::VolumeAndPanPlugin, te::LevelMeterPlugin, te::AuxSendPlugin, te::AuxReturnPlugin, te::VCAPlugin,
                       te::FreezePointPlugin, LoudnessMeterPlugin> (plugin);
    }

    juce::Array<te::Plugin*> getDevicesIn (const te::PluginList& list)
    {
        juce::Array<te::Plugin*> devices;

        for (auto* plugin : list)
            if (isDevice (*plugin))
                devices.add (plugin);

        return devices;
    }

    bool isDeviceState (const juce::ValueTree& state)
    {
        return state.hasType (schema::DEVICE) || state.hasType (schema::CONTAINER);
    }

    // The song's devices of a chain, as Tracktion's plug-ins in its list, in the song's order; Tracktion's list's own for an
    // Edit that isn't a song's (a preview's).
    juce::Array<te::Plugin*> getChainDevices (const te::PluginList& list, const juce::ValueTree& chain)
    {
        if (! chain.isValid())
            return getDevicesIn (list);

        juce::Array<te::Plugin*> devices;

        for (const auto& state : getDeviceStates (chain))
            for (auto* plugin : list)
                if (getDeviceID (*plugin) == getDeviceID (state))
                    devices.add (plugin);

        return devices;
    }

    // Puts a device's element among a chain's, before the one at a place, or after the last.
    void addDeviceState (te::Edit& edit, juce::ValueTree chain, const juce::ValueTree& device, int deviceIndex)
    {
        const auto devices = getDeviceStates (chain);
        auto index = chain.getNumChildren();

        if (juce::isPositiveAndBelow (deviceIndex, (int) devices.size()))
            index = chain.indexOf (devices[(size_t) deviceIndex]);
        else if (! devices.empty())
            index = chain.indexOf (devices.back()) + 1;

        chain.addChild (device, index, &edit.getUndoManager());
    }

    // Takes a device out of the song, and Tracktion's with it (in the caller's undo step), with what it holds of its state
    // stored first, so that an undo brings it back as it was.
    void removeDeviceState (te::Plugin& device)
    {
        if (auto state = findDeviceState (device.edit, getDeviceID (device)); state.isValid())
        {
            storeDeviceState (device);
            state.getParent().removeChild (state, &device.edit.getUndoManager());
            mirror::reconcileDevices (device.edit);
        }
    }

    // Sets a property without an undo step, only if its value is another (as a store does: DeviceStates.h).
    void store (juce::ValueTree& state, const juce::Identifier& property, const juce::var& value)
    {
        if (const auto* stored = state.getPropertyPointer (property); stored == nullptr || *stored != value)
            state.setProperty (property, value, nullptr);
    }

    // Adds a device to the song and makes it in Tracktion, which can't make a device from the song yet (T1, step 5).
    te::Plugin* insertInto (te::PluginList& list, const te::Plugin::Ptr& plugin, int deviceIndex)
    {
        if (plugin == nullptr)
            return nullptr;

        const auto index = isInstrument (*plugin) ? 0 : deviceIndex; // instruments come first
        list.insertPlugin (plugin, getDevicePlace (list, index), nullptr);

        if (! list.contains (plugin.get()))
            return nullptr;

        if (auto chain = findChainState (plugin->edit, list.getOwnerTrack()); chain.isValid())
            addDeviceState (plugin->edit, chain, makeDeviceState (*plugin), index);

        return plugin.get();
    }

    DeviceType makeBuiltIn (const juce::String& name, const juce::String& xmlType, bool instrument, const juce::String& maker)
    {
        juce::PluginDescription description;
        description.name = name;
        description.pluginFormatName = te::PluginManager::builtInPluginFormatName;
        description.category = instrument ? "Synth" : "Effect";
        description.manufacturerName = maker;
        description.fileOrIdentifier = xmlType;
        description.isInstrument = instrument;
        return makeDeviceType (description);
    }

    // Phazite's own instruments, or its effects.
    void addOwnDevices (std::vector<DeviceType>& types, devices::DeviceKind kind)
    {
        for (const auto* info : getOwnDevices())
            if (info->kind == kind)
                types.push_back (makeBuiltIn (juce::String (info->name.data(), info->name.size()),
                                              juce::String (info->typeID.data(), info->typeID.size()),
                                              kind == devices::DeviceKind::instrument, "Phazite"));
    }
} // namespace

//==============================================================================
bool isDevice (const te::Plugin& plugin)
{
    return ! isChannelPlugin (plugin);
}

bool opensWindow (const te::Plugin& device)
{
    return dynamic_cast<const te::ExternalPlugin*> (&device) != nullptr;
}

bool popsOut (const te::Plugin& device)
{
    return dynamic_cast<const DevicePlugin*> (&device) != nullptr;
}

bool isWindowOpen (const te::Plugin& device)
{
    return device.windowState != nullptr && device.windowState->isWindowShowing();
}

juce::Array<te::Plugin*> getDevices (const te::Track& track)
{
    return getChainDevices (track.pluginList, findChainState (track.edit, &track));
}

juce::Array<te::Plugin*> getMasterDevices (const te::Edit& edit)
{
    return getChainDevices (edit.getMasterPluginList(), findChainState (edit, nullptr));
}

te::Plugin* getInstrument (const te::Track& track)
{
    const auto devices = getDevices (track);
    return ! devices.isEmpty() && isInstrument (*devices.getFirst()) ? devices.getFirst() : nullptr;
}

juce::Array<te::Plugin*> getEffects (const te::Track& track)
{
    auto devices = getDevices (track);
    devices.removeFirstMatchingValue (getInstrument (track));
    return devices;
}

bool needsInstrument (const te::Track& track)
{
    auto* audioTrack = dynamic_cast<const te::AudioTrack*> (&track);

    if (audioTrack == nullptr || getReturnBus (track) >= 0 || getInstrument (track) != nullptr)
        return false;

    for (auto* clip : audioTrack->getClips())
        if (dynamic_cast<te::MidiClip*> (clip) != nullptr)
            return true;

    return false;
}

te::Plugin* setInstrument (te::AudioTrack& track, const te::Plugin::Ptr& plugin)
{
    if (plugin == nullptr || ! isInstrument (*plugin) || getReturnBus (track) >= 0)
        return nullptr;

    if (auto* old = getInstrument (track))
        removeDeviceState (*old);

    return insertInto (track.pluginList, plugin, 0);
}

te::Plugin* insertDevice (te::Track& track, const te::Plugin::Ptr& plugin, int index)
{
    if (auto* folder = dynamic_cast<te::FolderTrack*> (&track); folder != nullptr && ! isBus (*folder))
        makeBus (*folder);

    return insertInto (track.pluginList, plugin, index);
}

te::Plugin* insertMasterDevice (te::Edit& edit, const te::Plugin::Ptr& plugin, int index)
{
    return insertInto (edit.getMasterPluginList(), plugin, index);
}

void removeDevice (te::Plugin& device)
{
    const UndoStep step (device.edit, "Remove " + device.getName());
    auto* folder = dynamic_cast<te::FolderTrack*> (device.getOwnerTrack());

    // A container goes with what it holds (its rack type stays: Containers.h), and the devices after a device in a
    // container take its place.
    removeDeviceState (device);

    if (folder != nullptr && isBus (*folder) && getDevices (*folder).isEmpty()
        && ! findTrackState (*folder).getProperty (schema::folderBus, false))
        makePlainFolder (*folder);
}

void moveDevice (te::Plugin& device, int newIndex)
{
    const UndoStep step (device.edit, "Move Device");
    auto state = findDeviceState (device.edit, getDeviceID (device));

    if (! state.isValid())
        return;

    // To the place of the device that is there now: after it when moving down, before it when moving up. Clips, lanes
    // and a folder's tracks can sit between devices in the track's element.
    auto chain = state.getParent();
    const auto devices = getDeviceStates (chain);
    const auto& target = devices[(size_t) juce::jlimit (0, (int) devices.size() - 1, newIndex)];
    chain.moveChild (chain.indexOf (state), chain.indexOf (target), &device.edit.getUndoManager());
    mirror::reconcileDevices (device.edit);
}

juce::StringArray getKeyControls (te::Plugin& device, int maximum)
{
    juce::StringArray ids;

    // A container's own parameters are Tracktion's levels in and out, which Phazite doesn't show; Phazite's own devices show
    // theirs in their editors.
    if (isContainer (device) || dynamic_cast<DevicePlugin*> (&device) != nullptr)
        return ids;

    if (auto quick = device.getQuickControlParameter(); quick != nullptr && ! isMixLevel (*quick))
        ids.add (quick->paramID);

    for (auto* parameter : device.getAutomatableParameters())
        if (ids.size() < maximum && ! isMixLevel (*parameter))
            ids.addIfNotAlreadyThere (parameter->paramID);

    return ids;
}

bool hasMix (te::Plugin& device)
{
    return findOwnMix (device) != nullptr || findMixLevels (device).has_value();
}

float getMix (te::Plugin& device)
{
    if (const auto own = findOwnMix (device))
        return own->getCurrentValue();

    const auto levels = findMixLevels (device);

    if (! levels.has_value())
        return 1.0f;

    const auto wet = levels->wet->getCurrentValue(), dry = levels->dry->getCurrentValue();
    return wet + dry > 0.0f ? juce::jlimit (0.0f, 1.0f, wet / (wet + dry)) : 1.0f;
}

void setMix (te::Plugin& device, float mix)
{
    if (const auto own = findOwnMix (device))
    {
        setDeviceValue (*own, juce::jlimit (0.0f, 1.0f, mix));
        return;
    }

    if (const auto levels = findMixLevels (device))
    {
        const auto wet = juce::jlimit (0.0f, 1.0f, mix);

        // A plug-in's or a container's, in the song.
        if (auto state = findDeviceState (device.edit, getDeviceID (device)); isPluginState (state) || state.hasType (schema::CONTAINER))
        {
            if (wet < 1.0f)
                state.setProperty (schema::deviceMix, wet, &device.edit.getUndoManager());
            else
                state.removeProperty (schema::deviceMix, &device.edit.getUndoManager());
        }

        setDeviceValue (*levels->wet, wet);
        setDeviceValue (*levels->dry, 1.0f - wet);
    }
}

te::AutomatableParameter::Array getMixParameters (te::Plugin& device)
{
    te::AutomatableParameter::Array parameters;

    if (const auto own = findOwnMix (device))
        parameters.add (own);
    else if (const auto levels = findMixLevels (device))
    {
        parameters.add (levels->dry);
        parameters.add (levels->wet);
    }

    return parameters;
}

//==============================================================================
DeviceType makeDeviceType (const juce::PluginDescription& description)
{
    return { description.name, description.isInstrument ? "Instrument" : "Effect", description, description.isInstrument,
             te::PluginManager::isBuiltInPlugin (description) };
}

std::vector<DeviceType> getDeviceTypes (te::Engine& engine)
{
    // Phazite's own devices (roadmap step 1.6), instruments first, then the plug-ins found by scanning. Tracktion's
    // instruments and effects lock, allocate or read message-thread state on the audio thread, so Phazite doesn't use them
    // (docs/realtime-audit.md, finding 16).
    std::vector<DeviceType> types;
    addOwnDevices (types, devices::DeviceKind::instrument);
    addOwnDevices (types, devices::DeviceKind::effect);

    for (const auto& description : engine.getPluginManager().knownPluginList.getTypes())
        types.push_back (makeDeviceType (description));

    return types;
}

te::Plugin::Ptr createDevice (te::Edit& edit, const DeviceType& type)
{
    if (te::PluginManager::isBuiltInPlugin (type.description))
        return edit.getPluginCache().createNewPlugin (type.description.fileOrIdentifier, type.description);

    return edit.getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, type.description);
}

te::Plugin* chooseInstrument (te::AudioTrack& track, const DeviceType& type)
{
    const UndoStep step (track.edit, (getInstrument (track) != nullptr ? "Replace Instrument with " : "Add ") + type.name);
    return setInstrument (track, createDevice (track.edit, type));
}

te::Plugin* addEffect (te::Edit& edit, te::Track* track, const DeviceType& type)
{
    const UndoStep step (edit, "Add " + type.name);
    auto device = createDevice (edit, type);
    return track != nullptr ? insertDevice (*track, device) : insertMasterDevice (edit, device);
}

te::AudioTrack* addReturnWith (te::Track& sender, const DeviceType& type)
{
    const UndoStep step (sender.edit, "Add " + type.name + " Return");
    return addReturnWithEffect (sender, createDevice (sender.edit, type));
}

te::Plugin* addDeviceTo (te::Edit& edit, te::Track* track, const DeviceType& type, Selection* selection)
{
    const UndoStep step (edit, "Add " + type.name);
    return placeDevice (edit, track, createDevice (edit, type), selection);
}

te::Plugin* placeDevice (te::Edit& edit, te::Track* track, const te::Plugin::Ptr& device, Selection* selection)
{
    if (device == nullptr)
        return nullptr;

    if (! isInstrument (*device))
        return track != nullptr ? insertDevice (*track, device) : insertMasterDevice (edit, device);

    // An instrument silences a track's audio clips in Tracktion, so a track with audio gets one next to it instead.
    auto* audioTrack = dynamic_cast<te::AudioTrack*> (track);
    const auto holdsAudio = audioTrack != nullptr
                            && std::any_of (audioTrack->getClips().begin(), audioTrack->getClips().end(),
                                            [] (te::Clip* clip) { return dynamic_cast<te::AudioClipBase*> (clip) != nullptr; });

    if (audioTrack == nullptr || isReturnTrack (*audioTrack) || holdsAudio)
    {
        audioTrack = insertTrack (edit, track, selection);

        if (audioTrack == nullptr)
            return nullptr;

        setTrackName (*audioTrack, device->getName());

        if (const auto colour = getColourForName (device->getName()))
            setTrackColour (*audioTrack, *colour);
    }

    return setInstrument (*audioTrack, device);
}

//==============================================================================
void setMixedAsBus (te::FolderTrack& folder, bool bus)
{
    const UndoStep step (folder.edit, bus ? "Mix Folder as Bus" : "Make Folder Plain");

    if (bus)
    {
        mixAsBus (folder);
    }
    else if (getDevices (folder).isEmpty())
    {
        findTrackState (folder).removeProperty (schema::folderBus, &folder.edit.getUndoManager());
        makePlainFolder (folder);
    }
}

te::Plugin* getVolumeDevice (te::Track& track)
{
    if (auto* audio = dynamic_cast<te::AudioTrack*> (&track))
        return audio->getVolumePlugin();

    if (auto* folder = dynamic_cast<te::FolderTrack*> (&track))
        return isBus (*folder) ? static_cast<te::Plugin*> (folder->getVolumePlugin()) : static_cast<te::Plugin*> (folder->getVCAPlugin());

    return nullptr;
}

juce::String getVolumeParameterID (te::Track& track)
{
    if (auto* volume = dynamic_cast<te::VolumeAndPanPlugin*> (getVolumeDevice (track)))
        return volume->volParam->paramID;

    if (auto* vca = dynamic_cast<te::VCAPlugin*> (getVolumeDevice (track)))
        return vca->volParam->paramID;

    return {};
}

te::LevelMeasurer* getLevelMeasurer (te::Track& track)
{
    if (auto* audio = dynamic_cast<te::AudioTrack*> (&track))
        if (auto* meter = audio->getLevelMeterPlugin())
            return &meter->measurer;

    if (isBus (track))
        if (auto* meter = track.pluginList.findFirstPluginOfType<te::LevelMeterPlugin>())
            return &meter->measurer;

    return nullptr;
}

te::FolderTrack* getOutputBus (const te::Track& track)
{
    for (auto* parent = getParentFolder (track); parent != nullptr; parent = getParentFolder (*parent))
        if (isBus (*parent))
            return parent;

    return nullptr;
}

juce::Array<te::Track*> getChannelTracks (const te::Edit& edit)
{
    auto tracks = getPlaylistTracks (edit);

    for (auto* returnTrack : getReturnTracks (edit))
        tracks.addIfNotAlreadyThere (returnTrack);

    return tracks;
}

//==============================================================================
int getReturnBus (const te::Track& track)
{
    if (auto* input = track.pluginList.findFirstPluginOfType<te::AuxReturnPlugin>())
        return input->busNumber;

    return -1;
}

juce::Array<te::AudioTrack*> getReturnTracks (const te::Edit& edit)
{
    juce::Array<te::AudioTrack*> returns;

    for (auto* track : getPlaylistTracks (edit))
        if (auto* audio = dynamic_cast<te::AudioTrack*> (track); audio != nullptr && isReturnTrack (*audio))
            returns.add (audio);

    return returns;
}

te::AudioTrack* findReturnTrack (const te::Edit& edit, int bus)
{
    for (auto* track : getReturnTracks (edit))
        if (getReturnBus (*track) == bus)
            return track;

    return nullptr;
}

te::AudioTrack* addNewReturnTrack (te::Edit& edit, Selection* selection)
{
    return addReturnTrack (edit, "Return " + juce::String (getReturnTracks (edit).size() + 1), selection);
}

te::AudioTrack* addReturnTrack (te::Edit& edit, const juce::String& name, Selection* selection)
{
    // The first bus no return uses.
    auto bus = 0;

    while (findReturnTrack (edit, bus) != nullptr)
        ++bus;

    // Solo-safe, as Studio One's effect channels are: soloing a track keeps its reverb, say, audible (a return's solo
    // isolation, which the mirror gives it).
    auto* track = insertReturnTrack (edit, name, selection);

    if (track == nullptr)
        return nullptr;

    auto input = edit.getPluginCache().createNewPlugin (te::AuxReturnPlugin::xmlTypeName, {});

    if (auto* aux = dynamic_cast<te::AuxReturnPlugin*> (input.get()))
        aux->busNumber = bus;

    track->pluginList.insertPlugin (input, 0, nullptr);
    edit.setAuxBusName (bus, name);
    return track;
}

te::AudioTrack* addReturnWithEffect (te::Track& sender, const te::Plugin::Ptr& effect)
{
    if (effect == nullptr || isInstrument (*effect))
        return nullptr;

    // Named after the effect: "Reverb", or "Reverb 2" if there is one already.
    auto& edit = sender.edit;
    const auto returns = getReturnTracks (edit);
    const auto isTaken = [&returns] (const juce::String& candidate)
    { return std::any_of (returns.begin(), returns.end(), [&candidate] (te::AudioTrack* r) { return getTrackName (*r) == candidate; }); };

    auto name = effect->getName();

    for (auto number = 2; isTaken (name); ++number)
        name = effect->getName() + " " + juce::String (number);

    auto* created = addReturnTrack (edit, name, nullptr);

    if (created == nullptr)
        return nullptr;

    insertDevice (*created, effect);

    // The dry sound reaches the master through the track itself, so a return plays only its effect.
    if (hasMix (*effect))
        setMix (*effect, 1.0f);

    addSend (sender, getReturnBus (*created), false);
    return created;
}

juce::Array<te::AuxSendPlugin*> getSends (const te::Track& track)
{
    return track.pluginList.getPluginsOfType<te::AuxSendPlugin>();
}

te::AuxSendPlugin* addSend (te::Track& track, int bus, bool preFader)
{
    auto plugin = track.edit.getPluginCache().createNewPlugin (te::AuxSendPlugin::xmlTypeName, {});
    auto* send = dynamic_cast<te::AuxSendPlugin*> (plugin.get());
    auto* volume = getVolumeDevice (track);

    if (send == nullptr || volume == nullptr)
        return nullptr;

    send->busNumber = bus;
    send->setGainDb (newSendDb);

    // Just before the fader, or just after it.
    const auto volumeIndex = track.pluginList.indexOf (volume);
    track.pluginList.insertPlugin (plugin, preFader ? volumeIndex : volumeIndex + 1, nullptr);
    return track.pluginList.contains (send) ? send : nullptr;
}

bool isPreFader (const te::AuxSendPlugin& send)
{
    auto* track = send.getOwnerTrack();
    auto* volume = track != nullptr ? getVolumeDevice (*track) : nullptr;
    return volume != nullptr && track->pluginList.indexOf (&send) < track->pluginList.indexOf (volume);
}

void setDeviceOn (te::Plugin& device, bool on)
{
    if (isDeviceOn (device) == on)
        return;

    const UndoStep step (device.edit, on ? "Turn Device On" : "Turn Device Off");

    // A device of an Edit that isn't a song's (a preview's) is Tracktion's alone.
    if (auto state = findDeviceState (device.edit, getDeviceID (device)); state.isValid())
    {
        if (on)
            state.removeProperty (schema::deviceOff, &device.edit.getUndoManager());
        else
            state.setProperty (schema::deviceOff, true, &device.edit.getUndoManager());

        mirror::reconcileDevices (device.edit);
    }
    else
    {
        device.setEnabled (on);
    }
}

bool isDeviceOn (const te::Plugin& device)
{
    if (const auto state = findDeviceState (device.edit, getDeviceID (device)); state.isValid())
        return ! (bool) state[schema::deviceOff];

    return device.isEnabled();
}

juce::ValueTree findChainState (const te::Edit& edit, const te::Track* track)
{
    return track != nullptr ? findTrackState (*track) : findPhaziteState (edit).getChildWithName (schema::MASTER);
}

std::vector<juce::ValueTree> getDeviceStates (const juce::ValueTree& chain)
{
    std::vector<juce::ValueTree> devices;

    for (const auto& child : chain)
        if (isDeviceState (child))
            devices.push_back (child);

    return devices;
}

juce::ValueTree findDeviceState (const te::Edit& edit, DeviceID id)
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

    for (const auto& track : getTrackStates (edit))
        if (const auto found = findIn (track); found.isValid())
            return found;

    return findIn (findChainState (edit, nullptr));
}

DeviceID getDeviceID (const juce::ValueTree& deviceState)
{
    return DeviceID::fromRaw ((std::uint64_t) (juce::int64) deviceState[schema::deviceID]);
}

bool isPluginState (const juce::ValueTree& deviceState)
{
    return deviceState.hasType (schema::DEVICE) && deviceState[schema::deviceType].toString() == schema::deviceIsPlugin;
}

juce::ValueTree makeDeviceState (te::Plugin& device)
{
    juce::ValueTree state (isContainer (device) ? schema::CONTAINER : schema::DEVICE);
    state.setProperty (schema::deviceID, (juce::int64) device.itemID.getRawID(), nullptr);

    // A plug-in is the one kind of device with a window of its own.
    if (state.hasType (schema::DEVICE))
        state.setProperty (schema::deviceType,
                           opensWindow (device) ? juce::String (schema::deviceIsPlugin) : device.state[te::IDs::type].toString(), nullptr);

    if (! device.isEnabled())
        state.setProperty (schema::deviceOff, true, nullptr);

    // A plug-in: which it is, and what it was made with (a preset's settings, its mix), as its state has them. A save stores
    // what it holds then (DeviceStates.h).
    if (auto* plugin = dynamic_cast<te::ExternalPlugin*> (&device))
    {
        setPluginIdentity (state, plugin->desc);

        if (const auto settings = device.state[te::IDs::state].toString(); settings.isNotEmpty())
            state.setProperty (schema::pluginState, settings, nullptr);

        if (device.state.hasProperty (te::IDs::programNum))
            state.setProperty (schema::pluginProgram, (int) device.state[te::IDs::programNum], nullptr);

        if (const auto mix = getMix (device); mix < 1.0f)
            state.setProperty (schema::deviceMix, mix, nullptr);
    }

    // One of Phazite's own: its values set by hand and its files, as its state has them.
    if (auto* own = dynamic_cast<const DevicePlugin*> (&device))
    {
        juce::ValueTree values (schema::VALUES), sounds (schema::SOUNDS);

        for (const auto& description : own->getDeviceInfo().parameters)
            if (const juce::Identifier id (juce::String (description.id.data(), description.id.size())); device.state.hasProperty (id))
                values.setProperty (id, device.state[id], nullptr);

        for (const auto& property : getSoundProperties (own->getDeviceInfo()))
            if (device.state.hasProperty (property))
                sounds.setProperty (property, device.state[property], nullptr);

        if (values.getNumProperties() > 0)
            state.appendChild (values, nullptr);

        if (sounds.getNumProperties() > 0)
            state.appendChild (sounds, nullptr);
    }

    // A container: its name, its mix, and its devices in their order (a preset's, as it made them).
    if (auto* rack = getContainerRack (device))
    {
        state.setProperty (schema::containerName, rack->rackName.get(), nullptr);

        if (const auto mix = getMix (device); mix < 1.0f)
            state.setProperty (schema::deviceMix, mix, nullptr);

        for (auto* inside : getContainerDevices (device))
            state.appendChild (makeDeviceState (*inside), nullptr);
    }

    return state;
}

void setPluginIdentity (juce::ValueTree& device, const juce::PluginDescription& description)
{
    store (device, schema::pluginFormat, description.pluginFormatName);
    store (device, schema::pluginName, description.name);
    store (device, schema::pluginMaker, description.manufacturerName);
    store (device, schema::pluginIdentifier, description.fileOrIdentifier);
    store (device, schema::pluginUid, description.uniqueId);
}

std::optional<juce::PluginDescription> findKnownPlugin (te::Engine& engine, const juce::PluginDescription& wanted)
{
    const auto types = engine.getPluginManager().knownPluginList.getTypes();

    for (const auto& type : types)
        if (wanted.fileOrIdentifier.isNotEmpty() && type.fileOrIdentifier == wanted.fileOrIdentifier && type.name == wanted.name)
            return type;

    for (const auto& type : types)
        if (type.name == wanted.name && type.manufacturerName == wanted.manufacturerName
            && type.pluginFormatName == wanted.pluginFormatName)
            return type;

    return {};
}

juce::PluginDescription getPluginDescription (te::Engine& engine, const juce::ValueTree& device)
{
    juce::PluginDescription description;
    description.pluginFormatName = device[schema::pluginFormat].toString();
    description.name = device[schema::pluginName].toString();
    description.manufacturerName = device[schema::pluginMaker].toString();
    description.fileOrIdentifier = device[schema::pluginIdentifier].toString();
    description.uniqueId = (int) device[schema::pluginUid];
    return findKnownPlugin (engine, description).value_or (description);
}

int getDevicePlace (const te::PluginList& list, int deviceIndex)
{
    const auto devices = getDevicesIn (list);

    if (juce::isPositiveAndBelow (deviceIndex, devices.size()))
        return list.indexOf (devices[deviceIndex]);

    auto index = 0;

    for (int i = 0; i < list.size(); ++i)
        if (isDevice (*list[i]) || dynamic_cast<te::AuxReturnPlugin*> (list[i]) != nullptr)
            index = i + 1;

    return index;
}

void setDeviceValue (te::AutomatableParameter& parameter, float value)
{
    const auto stored = parameter.snapToState (parameter.getValueRange().clipValue (value));

    if (auto* own = dynamic_cast<DevicePlugin*> (parameter.getPlugin()))
    {
        if (auto state = findDeviceState (own->edit, getDeviceID (*own)); state.isValid())
        {
            auto* undo = &own->edit.getUndoManager();
            state.getOrCreateChildWithName (schema::VALUES, undo).setProperty (juce::Identifier (parameter.paramID), stored, undo);
        }
    }

    parameter.setParameter (stored, juce::sendNotification);
}

void setDeviceSound (te::Plugin& device, const juce::Identifier& property, const juce::String& value)
{
    auto* undo = &device.edit.getUndoManager();

    if (auto state = findDeviceState (device.edit, getDeviceID (device)); state.isValid())
    {
        if (value.isNotEmpty())
            state.getOrCreateChildWithName (schema::SOUNDS, undo).setProperty (property, value, undo);
        else if (auto sounds = state.getChildWithName (schema::SOUNDS); sounds.isValid())
            sounds.removeProperty (property, undo);
    }

    if (value.isNotEmpty())
        device.state.setProperty (property, value, undo);
    else
        device.state.removeProperty (property, undo);
}

void giveCopiedDeviceIDs (juce::ValueTree& copiedChain, const te::Track& copy, const te::EditItemID::IDMap& inContainers)
{
    const auto devices = getDevicesIn (copy.pluginList); // Tracktion's copies, in the original's order
    const auto states = getDeviceStates (copiedChain);

    for (size_t i = 0; i < states.size() && (int) i < devices.size(); ++i)
    {
        juce::ValueTree (states[i]).setProperty (schema::deviceID, (juce::int64) devices[(int) i]->itemID.getRawID(), nullptr);

        for (auto inside : getDeviceStates (states[i]))
            if (const auto copied = inContainers.find (toTracktion (getDeviceID (inside))); copied != inContainers.end())
                inside.setProperty (schema::deviceID, (juce::int64) copied->second.getRawID(), nullptr);
    }
}

void rememberWindowPosition (te::Plugin& device, juce::Point<int> topLeft)
{
    device.state.setProperty (te::IDs::windowX, topLeft.x, nullptr);
    device.state.setProperty (te::IDs::windowY, topLeft.y, nullptr);
}

te::AuxSendPlugin* addSendTo (te::Track& track, int bus)
{
    const UndoStep step (track.edit, "Add Send");
    return addSend (track, bus, false);
}

te::AuxSendPlugin* addSendToNewReturn (te::Track& track)
{
    const UndoStep step (track.edit, "Add Send");
    auto* created = addNewReturnTrack (track.edit, nullptr);
    return created != nullptr ? addSend (track, getReturnBus (*created), false) : nullptr;
}

void removeSend (te::AuxSendPlugin& send)
{
    const UndoStep step (send.edit, "Remove Send");
    send.deleteFromParent();
}

void setPreFader (te::AuxSendPlugin& send, bool preFader)
{
    auto* track = send.getOwnerTrack();
    auto* volume = track != nullptr ? getVolumeDevice (*track) : nullptr;

    if (volume == nullptr || isPreFader (send) == preFader)
        return;

    const UndoStep step (send.edit, "Change Send");

    // Moving it to the fader's place puts it just before the fader when it comes from after, and just after
    // it when it comes from before.
    auto parent = send.state.getParent();
    parent.moveChild (parent.indexOf (send.state), parent.indexOf (volume->state), &send.edit.getUndoManager());
}

//==============================================================================
void addMixerCommands (CommandRegistry& registry, Selection& selection)
{
    registry.add ({ .id = "track.addReturn",
                    .name = "Add Return Track",
                    .category = "Track",
                    .description = "Adds a return track, for an effect such as a reverb that several tracks send to.",
                    .synonyms = { "aux", "bus", "send", "fx return", "reverb" },
                    .perform = withEdit (selection, [&selection] (te::Edit& edit) { addNewReturnTrack (edit, &selection); }),
                    .isEnabled = [&selection] { return selection.getEdit() != nullptr; },
                    .undoTarget = undoTargetOf (selection) });
}
} // namespace phazite
