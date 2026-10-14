// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Mixer.h"
#include "model/arrangement/Import.h"
#include "model/arrangement/Tracks.h"
#include "model/engine/LoudnessMeterPlugin.h"

#include <optional>

namespace phazite
{
namespace
{
    constexpr float newSendDb = -6.0f;
    const juce::Identifier busSwitchProperty { "phaziteBus" }; // a folder made a bus with the bus switch

    // A device's dry and wet levels, and the value of each at which it passes its sound at full level: 1 for Tracktion's
    // plug-ins, and a half (dry) and a third (wet) for its reverb, which scales them up.
    struct MixLevels
    {
        te::AutomatableParameter::Ptr dry, wet;
        float dryAtFull = 1.0f, wetAtFull = 1.0f;
    };

    std::optional<MixLevels> findMixLevels (te::Plugin& device)
    {
        if (device.isSynth())
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

        list.insertPlugin (plugin, getInsertIndex (list, plugin->isSynth() ? 0 : deviceIndex), nullptr); // instruments come first
        return list.contains (plugin.get()) ? plugin.get() : nullptr;
    }

    DeviceType makeBuiltIn (const char* name, const char* xmlType, bool isInstrument)
    {
        juce::PluginDescription description;
        description.name = name;
        description.pluginFormatName = te::PluginManager::builtInPluginFormatName;
        description.category = isInstrument ? "Synth" : "Effect";
        description.manufacturerName = "Tracktion";
        description.fileOrIdentifier = xmlType;
        description.isInstrument = isInstrument;
        return { name, isInstrument ? "Instrument" : "Effect", description, isInstrument };
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
    return ! devices.isEmpty() && devices.getFirst()->isSynth() ? devices.getFirst() : nullptr;
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
    if (plugin == nullptr || ! plugin->isSynth() || getReturnBus (track) >= 0)
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
    auto* folder = dynamic_cast<te::FolderTrack*> (device.getOwnerTrack());
    device.deleteFromParent();

    if (folder != nullptr && isBus (*folder) && getDevices (*folder).isEmpty() && ! folder->state.getProperty (busSwitchProperty, false))
        makePlainFolder (*folder);
}

void moveDevice (te::Plugin& device, int newIndex)
{
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

    if (auto quick = device.getQuickControlParameter(); quick != nullptr && ! isMixLevel (*quick))
        ids.add (quick->paramID);

    for (auto* parameter : device.getAutomatableParameters())
        if (ids.size() < maximum && ! isMixLevel (*parameter))
            ids.addIfNotAlreadyThere (parameter->paramID);

    return ids;
}

bool hasMix (te::Plugin& device)
{
    return findMixLevels (device).has_value();
}

float getMix (te::Plugin& device)
{
    const auto levels = findMixLevels (device);

    if (! levels.has_value())
        return 1.0f;

    const auto wet = levels->wet->getCurrentValue() / levels->wetAtFull;
    const auto dry = levels->dry->getCurrentValue() / levels->dryAtFull;
    return wet + dry > 0.0f ? juce::jlimit (0.0f, 1.0f, wet / (wet + dry)) : 1.0f;
}

void setMix (te::Plugin& device, float mix)
{
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

    if (const auto levels = findMixLevels (device))
    {
        parameters.add (levels->dry);
        parameters.add (levels->wet);
    }

    return parameters;
}

//==============================================================================
std::vector<DeviceType> getDeviceTypes (te::Engine& engine)
{
    // Until Phazite's own devices arrive (roadmap step 1.6), two of Tracktion's: the 4OSC synth, which the starter
    // project uses, and the reverb, which follows the real-time rules. Tracktion's other devices lock, allocate or
    // read message-thread state on the audio thread, so they aren't offered (docs/realtime-audit.md, finding 16).
    std::vector<DeviceType> types { makeBuiltIn ("4OSC synth", te::FourOscPlugin::xmlTypeName, true),
                                    makeBuiltIn ("Reverb", te::ReverbPlugin::xmlTypeName, false) };

    for (const auto& description : engine.getPluginManager().knownPluginList.getTypes())
        types.push_back ({ description.name, description.isInstrument ? "Instrument" : "Effect", description, description.isInstrument });

    return types;
}

te::Plugin::Ptr createDevice (te::Edit& edit, const DeviceType& type)
{
    if (te::PluginManager::isBuiltInPlugin (type.description))
        return edit.getPluginCache().createNewPlugin (type.description.fileOrIdentifier, type.description);

    return edit.getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, type.description);
}

te::Plugin* addDeviceTo (te::Edit& edit, te::Track* track, const DeviceType& type, te::SelectionManager* selection)
{
    edit.getUndoManager().beginNewTransaction ("Add " + type.name);
    auto device = createDevice (edit, type);

    if (device == nullptr)
        return nullptr;

    if (! type.isInstrument)
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

        audioTrack->setName (type.name);

        if (const auto colour = getColourForName (type.name))
            audioTrack->setColour (*colour);
    }

    return setInstrument (*audioTrack, device);
}

//==============================================================================
bool isBus (const te::Track& track)
{
    auto* folder = dynamic_cast<const te::FolderTrack*> (&track);
    return folder != nullptr && folder->isSubmixFolder();
}

void makeBus (te::FolderTrack& folder)
{
    if (isBus (folder))
        return;

    // The bus's fader starts where the folder's VCA was. Its volume and meter go straight into the folder's
    // state: Tracktion's insertPlugin() warns that the folder becomes a bus, which is what this is for.
    auto& edit = folder.edit;
    auto* undoManager = &edit.getUndoManager();
    auto* vca = folder.getVCAPlugin();
    const auto volumeDb = vca != nullptr ? vca->getVolumeDb() : 0.0f;

    auto volume = edit.getPluginCache().createNewPlugin (te::VolumeAndPanPlugin::create());
    auto meter = edit.getPluginCache().createNewPlugin (te::LevelMeterPlugin::create());
    folder.state.addChild (volume->state, -1, undoManager);
    folder.state.addChild (meter->state, -1, undoManager);

    if (vca != nullptr)
        vca->deleteFromParent();

    if (auto* fader = dynamic_cast<te::VolumeAndPanPlugin*> (volume.get()))
        fader->setVolumeDb (volumeDb);
}

void makePlainFolder (te::FolderTrack& folder)
{
    if (! isBus (folder))
        return;

    auto* volume = folder.getVolumePlugin();
    const auto volumeDb = volume != nullptr ? volume->getVolumeDb() : 0.0f;

    for (auto* plugin : folder.pluginList.getPlugins()) // a copy that holds them while they are deleted
        if (dynamic_cast<te::TextPlugin*> (plugin) == nullptr)
            plugin->deleteFromParent();

    folder.pluginList.insertPlugin (te::VCAPlugin::create(), -1);

    if (auto* vca = folder.getVCAPlugin())
        vca->setVolumeDb (volumeDb);
}

void setMixedAsBus (te::FolderTrack& folder, bool bus)
{
    auto* undoManager = &folder.edit.getUndoManager();

    if (bus)
    {
        makeBus (folder);
        folder.state.setProperty (busSwitchProperty, true, undoManager);
    }
    else if (getDevices (folder).isEmpty())
    {
        folder.state.removeProperty (busSwitchProperty, undoManager);
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
    if (effect == nullptr || effect->isSynth())
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

    if (auto* reverb = dynamic_cast<te::ReverbPlugin*> (effect.get()))
        reverb->setDry (0.0f);

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

void setPreFader (te::AuxSendPlugin& send, bool preFader)
{
    auto* track = send.getOwnerTrack();
    auto* volume = track != nullptr ? getVolumeDevice (*track) : nullptr;

    if (volume == nullptr || isPreFader (send) == preFader)
        return;

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
                    .perform =
                        [&selection]
                    {
                        if (auto* edit = selection.getEdit())
                            addReturnTrack (*edit, "Return " + juce::String (getReturnTracks (*edit).size() + 1), &selection);
                    },
                    .isEnabled = [&selection] { return selection.getEdit() != nullptr; },
                    .undoTarget = [&selection] { return selection.getEdit(); } });
}
} // namespace phazite
