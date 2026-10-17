// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Mixer.h"
#include "internal/Containers.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DevicePlugin.h"
#include "model/devices/internal/Devices.h"
#include "model/engine/internal/LoudnessMeterPlugin.h"

#include <optional>

namespace phazite
{
namespace
{
    constexpr float newSendDb = -6.0f;

    // A device's dry and wet levels, and the value of each at which it passes its sound at full level: 1 for Tracktion's
    // plug-ins, and a half (dry) and a third (wet) for its reverb, which scales them up.
    struct MixLevels
    {
        te::AutomatableParameter::Ptr dry, wet;
        float dryAtFull = 1.0f, wetAtFull = 1.0f;
    };

    std::optional<MixLevels> findMixLevels (te::Plugin& device)
    {
        if (isInstrument (device))
            return {};

        auto dry = device.getAutomatableParameterByID ("dry level");
        auto wet = device.getAutomatableParameterByID ("wet level");

        if (dry == nullptr || wet == nullptr)
            return {};

        if (dynamic_cast<te::ReverbPlugin*> (&device) != nullptr)
            return MixLevels { dry, wet, 0.5f, 1.0f / 3.0f };

        return MixLevels { dry, wet, 1.0f, 1.0f };
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
        return isAnyOf<te::VolumeAndPanPlugin, te::LevelMeterPlugin, te::AuxSendPlugin, te::AuxReturnPlugin, te::VCAPlugin, te::TextPlugin,
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

    // Where in a channel's plug-ins a device goes: before the device at that place among its devices, or else
    // after the last device (or after a return's input), so before the sends, the fader and the meters.
    int getInsertIndex (const te::PluginList& list, int deviceIndex)
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

    te::Plugin* insertInto (te::PluginList& list, const te::Plugin::Ptr& plugin, int deviceIndex)
    {
        if (plugin == nullptr)
            return nullptr;

        list.insertPlugin (plugin, getInsertIndex (list, isInstrument (*plugin) ? 0 : deviceIndex), nullptr); // instruments come first
        return list.contains (plugin.get()) ? plugin.get() : nullptr;
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

juce::Array<te::Plugin*> getDevices (const te::Track& track)
{
    return getDevicesIn (track.pluginList);
}

juce::Array<te::Plugin*> getMasterDevices (const te::Edit& edit)
{
    return getDevicesIn (edit.getMasterPluginList());
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
        old->deleteFromParent();

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

    // A container goes with its rack type, and the devices after a device in a container take its place.
    if (auto* container = dynamic_cast<te::RackInstance*> (&device))
        deleteContainer (*container);
    else if (device.isInRack())
        deleteFromContainer (device);
    else
        device.deleteFromParent();

    if (folder != nullptr && isBus (*folder) && getDevices (*folder).isEmpty() && ! folder->state.getProperty (schema::phaziteBus, false))
        makePlainFolder (*folder);
}

void moveDevice (te::Plugin& device, int newIndex)
{
    const UndoStep step (device.edit, "Move Device");
    auto* list = device.getOwnerList();

    if (list == nullptr)
        return;

    // To the place of the device that is there now: after it when moving down, before it when moving up. Other
    // plug-ins (such as sends) and clips can sit between devices in the track's state.
    const auto devices = getDevicesIn (*list);

    if (! devices.contains (&device))
        return;

    auto parent = device.state.getParent();
    const auto& target = devices[juce::jlimit (0, devices.size() - 1, newIndex)]->state;
    parent.moveChild (parent.indexOf (device.state), parent.indexOf (target), &device.edit.getUndoManager());
}

juce::StringArray getKeyControls (te::Plugin& device, int maximum)
{
    juce::StringArray ids;

    // A container's own parameters are Tracktion's levels in and out, which Phazite doesn't show.
    if (isContainer (device))
        return ids;

    // Phazite's own devices name theirs.
    if (auto* own = dynamic_cast<DevicePlugin*> (&device))
    {
        for (const auto id : own->getDeviceInfo().keyControls)
            if (ids.size() < maximum)
                ids.add (juce::String (id.data(), id.size()));

        return ids;
    }

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

    const auto wet = levels->wet->getCurrentValue() / levels->wetAtFull;
    const auto dry = levels->dry->getCurrentValue() / levels->dryAtFull;
    return wet + dry > 0.0f ? juce::jlimit (0.0f, 1.0f, wet / (wet + dry)) : 1.0f;
}

void setMix (te::Plugin& device, float mix)
{
    if (const auto own = findOwnMix (device))
    {
        own->setParameter (juce::jlimit (0.0f, 1.0f, mix), juce::sendNotification);
        return;
    }

    if (const auto levels = findMixLevels (device))
    {
        const auto wet = juce::jlimit (0.0f, 1.0f, mix);
        levels->wet->setParameter (wet * levels->wetAtFull, juce::sendNotification);
        levels->dry->setParameter ((1.0f - wet) * levels->dryAtFull, juce::sendNotification);
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
    // Phazite's own devices (roadmap step 1.6), instruments first. Tracktion's lock, allocate or read message-thread state
    // on the audio thread, so they aren't offered (docs/realtime-audit.md, finding 16); projects that hold its 4OSC synth or
    // its reverb, which were offered until Phazite had its own, still open and play them.
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

te::Plugin* addDeviceTo (te::Edit& edit, te::Track* track, const DeviceType& type, te::SelectionManager* selection)
{
    const UndoStep step (edit, "Add " + type.name);
    return placeDevice (edit, track, createDevice (edit, type), selection);
}

te::Plugin* placeDevice (te::Edit& edit, te::Track* track, const te::Plugin::Ptr& device, te::SelectionManager* selection)
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

    if (audioTrack == nullptr || getReturnBus (*audioTrack) >= 0 || holdsAudio)
    {
        audioTrack = insertTrack (edit, track, selection);

        if (audioTrack == nullptr)
            return nullptr;

        audioTrack->setName (device->getName());

        if (const auto colour = getColourForName (device->getName()))
            audioTrack->setColour (*colour);
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
        folder.state.removeProperty (schema::phaziteBus, &folder.edit.getUndoManager());
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

    for (auto* track : te::getAudioTracks (edit))
        if (getReturnBus (*track) >= 0)
            returns.add (track);

    return returns;
}

te::AudioTrack* findReturnTrack (const te::Edit& edit, int bus)
{
    for (auto* track : getReturnTracks (edit))
        if (getReturnBus (*track) == bus)
            return track;

    return nullptr;
}

te::AudioTrack* addNewReturnTrack (te::Edit& edit, te::SelectionManager* selection)
{
    return addReturnTrack (edit, "Return " + juce::String (getReturnTracks (edit).size() + 1), selection);
}

te::AudioTrack* addReturnTrack (te::Edit& edit, const juce::String& name, te::SelectionManager* selection)
{
    // The first bus no return uses.
    auto bus = 0;

    while (findReturnTrack (edit, bus) != nullptr)
        ++bus;

    auto* track = insertTrack (edit, nullptr, selection);

    if (track == nullptr)
        return nullptr;

    track->setName (name);
    auto input = edit.getPluginCache().createNewPlugin (te::AuxReturnPlugin::xmlTypeName, {});

    if (auto* aux = dynamic_cast<te::AuxReturnPlugin*> (input.get()))
        aux->busNumber = bus;

    track->pluginList.insertPlugin (input, 0, nullptr);
    edit.setAuxBusName (bus, name);

    // Solo-safe, as Studio One's effect channels are: soloing a track keeps its reverb, say, audible.
    track->setSoloIsolate (true);
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
    { return std::any_of (returns.begin(), returns.end(), [&candidate] (te::AudioTrack* r) { return r->getName() == candidate; }); };

    auto name = effect->getName();

    for (auto number = 2; isTaken (name); ++number)
        name = effect->getName() + " " + juce::String (number);

    auto* created = addReturnTrack (edit, name, nullptr);

    if (created == nullptr)
        return nullptr;

    insertDevice (*created, effect);

    // The dry sound reaches the master through the track itself, so a return plays only its effect.
    if (auto* reverb = dynamic_cast<te::ReverbPlugin*> (effect.get()))
        reverb->setDry (0.0f);
    else if (findOwnMix (*effect) != nullptr)
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
    if (device.isEnabled() == on)
        return;

    const UndoStep step (device.edit, on ? "Turn Device On" : "Turn Device Off");
    device.setEnabled (on);
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
void addMixerCommands (CommandRegistry& registry, te::SelectionManager& selection)
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
