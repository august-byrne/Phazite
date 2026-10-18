// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Presets.h"
#include "PresetModel.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/Migrations.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DeviceFiles.h"
#include "model/devices/internal/Devices.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

namespace phazite
{
namespace
{
    constexpr const char* deviceKind = "device";
    constexpr const char* containerKind = "container";
    constexpr const char* trackKind = "track";

    std::optional<PresetKind> findKind (const juce::String& name)
    {
        if (name == deviceKind)
            return PresetKind::device;

        if (name == containerKind)
            return PresetKind::container;

        if (name == trackKind)
            return PresetKind::track;

        return {};
    }

    juce::ValueTree makePreset (const char* kind)
    {
        juce::ValueTree preset (schema::PHAZITEPRESET);
        preset.setProperty (schema::presetKind, kind, nullptr);
        preset.setProperty (schema::schemaVersion, schema::currentVersion, nullptr);
        return preset;
    }

    // Takes out of a device's state, and those of the devices in it, what ties it to its project: its automation, what
    // modulates it from outside the preset, its sidechain's source, where its window was, and whether it is on.
    void cleanForPreset (juce::ValueTree state, const juce::Array<te::EditItemID>& modulatorsInside)
    {
        for (int i = state.getNumChildren(); --i >= 0;)
        {
            auto child = state.getChild (i);

            if (child.hasType (te::IDs::AUTOMATIONCURVE))
            {
                state.removeChild (i, nullptr);
            }
            else if (child.hasType (te::IDs::MODIFIERASSIGNMENTS))
            {
                for (int j = child.getNumChildren(); --j >= 0;)
                    if (! modulatorsInside.contains (te::EditItemID::fromProperty (child.getChild (j), te::IDs::source)))
                        child.removeChild (j, nullptr);
            }
            else
            {
                cleanForPreset (child, modulatorsInside);
            }
        }

        if (! state.hasType (te::IDs::PLUGIN))
            return;

        for (const auto* property : { &te::IDs::sidechainSourceID, &te::IDs::enabled })
            state.removeProperty (*property, nullptr);

        for (int i = state.getNumProperties(); --i >= 0;)
            if (schema::isDeviceViewProperty (state.getPropertyName (i)))
                state.removeProperty (state.getPropertyName (i), nullptr);
    }

    // A state as a preset holds it: the values set by hand, not what automation or modulation set as the song played; the
    // sound files devices play (a drum sampler's, a wavetable synth's) by their full paths; and nothing that ties it to its
    // project.
    juce::ValueTree copyForPreset (const te::Edit& edit, const juce::ValueTree& state, const juce::Array<te::EditItemID>& modulatorsInside)
    {
        auto copy = state.createCopy();
        restoreValuesSetByHand (copy);
        writeDeviceFilePaths (edit, copy);
        cleanForPreset (copy, modulatorsInside);
        return copy;
    }

    juce::ValueTree copyDeviceForPreset (te::Plugin& device, const juce::Array<te::EditItemID>& modulatorsInside)
    {
        device.flushPluginStateToValueTree();
        return copyForPreset (device.edit, device.state, modulatorsInside);
    }

    // A container's rack type, with what its macros modulate in it, and what the modulators outside it given modulate.
    juce::ValueTree copyRackForPreset (te::RackType& rack, juce::Array<te::EditItemID> modulatorsInside)
    {
        for (auto* macro : rack.getMacroParameters())
            modulatorsInside.add (te::EditItemID::fromID (macro->state));

        rack.flushStateToValueTree();
        return copyForPreset (rack.edit, rack.state, modulatorsInside);
    }

    // Brings what a preset holds up to date as a project's state is (Migrations.h), in which it is wrapped meanwhile.
    bool migratePreset (juce::ValueTree& preset)
    {
        juce::ValueTree project (te::IDs::EDIT), phaziteState (schema::PHAZITE);
        phaziteState.setProperty (schema::schemaVersion, preset[schema::schemaVersion], nullptr);
        project.appendChild (phaziteState, nullptr);

        while (preset.getNumChildren() > 0)
        {
            auto child = preset.getChild (0);
            preset.removeChild (0, nullptr);
            project.appendChild (child, nullptr);
        }

        const auto upToDate = migrateToCurrentSchema (project);
        project.removeChild (phaziteState, nullptr);

        while (project.getNumChildren() > 0)
        {
            auto child = project.getChild (0);
            project.removeChild (0, nullptr);
            preset.appendChild (child, nullptr);
        }

        if (upToDate)
            preset.setProperty (schema::schemaVersion, schema::currentVersion, nullptr);

        return upToDate;
    }

    // A volume plug-in's volume and pan, from its state in a track preset, with what modulates them.
    void applyVolume (te::AudioTrack& track, const juce::ValueTree& saved)
    {
        auto* volume = track.getVolumePlugin();

        if (volume == nullptr)
            return;

        if (saved.hasProperty (te::IDs::volume))
            volume->volParam->setParameter ((float) saved[te::IDs::volume], juce::sendNotification);

        if (saved.hasProperty (te::IDs::pan))
            volume->panParam->setParameter ((float) saved[te::IDs::pan], juce::sendNotification);

        auto assignments = volume->state.getOrCreateChildWithName (te::IDs::MODIFIERASSIGNMENTS, &track.edit.getUndoManager());

        for (const auto& assignment : saved.getChildWithName (te::IDs::MODIFIERASSIGNMENTS))
            assignments.appendChild (assignment.createCopy(), &track.edit.getUndoManager());
    }
} // namespace

//==============================================================================
juce::ValueTree makeDevicePreset (te::Plugin& device)
{
    auto* rack = getContainerRack (device);
    auto preset = makePreset (rack != nullptr ? containerKind : deviceKind);

    if (rack != nullptr)
    {
        preset.appendChild (copyRackForPreset (*rack, {}), nullptr);
    }
    else
    {
        preset.setProperty (schema::presetDevice, device.getName(), nullptr);

        if (isInstrument (device))
            preset.setProperty (schema::presetInstrument, true, nullptr);
    }

    preset.appendChild (copyDeviceForPreset (device, {}), nullptr);
    return preset;
}

juce::ValueTree makeTrackPreset (te::Track& track)
{
    auto& edit = track.edit;
    auto preset = makePreset (trackKind);
    track.flushStateToValueTree();

    // Its modulators and macros, which may modulate what it holds.
    juce::Array<te::EditItemID> modulators;

    for (const auto& modulator : getModulators (track))
        modulators.add (phazite::toTracktion (modulator.getID()));

    juce::ValueTree saved (track.state.getType());
    saved.setProperty (te::IDs::name, track.getName(), nullptr);

    for (const auto* property : { &te::IDs::colour, &schema::phaziteDrumMode, &schema::phaziteDrumOrder, &schema::phaziteDrumHidden })
        if (track.state.hasProperty (*property))
            saved.setProperty (*property, track.state[*property], nullptr);

    for (const auto* list : { &te::IDs::MODIFIERS, &te::IDs::MACROPARAMETERS })
        if (const auto modulatorStates = track.state.getChildWithName (*list); modulatorStates.isValid())
            saved.appendChild (copyForPreset (edit, modulatorStates, modulators), nullptr);

    // Its volume and pan, and its devices with its containers' rack types: not its sends, meters or what goes into it.
    for (auto* plugin : track.pluginList)
    {
        if (dynamic_cast<te::VolumeAndPanPlugin*> (plugin) == nullptr && ! isDevice (*plugin))
            continue;

        saved.appendChild (copyDeviceForPreset (*plugin, modulators), nullptr);

        if (auto* rack = getContainerRack (*plugin))
            preset.appendChild (copyRackForPreset (*rack, modulators), nullptr);
    }

    preset.addChild (saved, 0, nullptr);
    return preset;
}

juce::ValueTree readPreset (const juce::File& file)
{
    const auto xml = file.existsAsFile() ? juce::parseXML (file) : nullptr;

    if (xml == nullptr)
        return {};

    auto preset = juce::ValueTree::fromXml (*xml);

    if (! preset.hasType (schema::PHAZITEPRESET) || ! findKind (preset[schema::presetKind]).has_value()
        || ! preset.hasProperty (schema::schemaVersion) || ! migratePreset (preset))
        return {};

    return preset;
}

bool writePreset (const juce::ValueTree& preset, const juce::File& file)
{
    const auto xml = preset.createXml();
    return xml != nullptr && file.getParentDirectory().createDirectory() && xml->writeTo (file);
}

te::Plugin* addDeviceFromPreset (te::Edit& edit, te::Track* channel, const juce::ValueTree& preset, te::SelectionManager* selection)
{
    // New IDs for all it holds, references among them included: a container's rack type, its devices and their wires,
    // and what its macros modulate.
    auto content = preset.createCopy();
    te::EditItemID::remapIDs (content, nullptr, edit);
    copyDeviceFilesInto (edit, content);

    auto device = content.getChildWithName (te::IDs::PLUGIN);
    const auto rack = content.getChildWithName (te::IDs::RACK);

    if (! device.isValid() || (rack.isValid() && addContainerRack (edit, rack) == nullptr))
        return nullptr;

    content.removeChild (device, nullptr); // the Edit takes the state itself
    return placeDevice (edit, channel, edit.getPluginCache().createNewPlugin (device), selection);
}

te::AudioTrack* addTrackFromPreset (te::Edit& edit, te::Track* after, const juce::ValueTree& preset, te::SelectionManager* selection)
{
    auto content = preset.createCopy();
    te::EditItemID::remapIDs (content, nullptr, edit);
    copyDeviceFilesInto (edit, content);
    const auto saved = content.getChildWithName (te::IDs::TRACK);

    if (! saved.isValid())
        return nullptr;

    // Its containers' rack types first, which its containers play.
    for (const auto& rack : content)
        if (rack.hasType (te::IDs::RACK))
            addContainerRack (edit, rack);

    auto* track = insertTrack (edit, after, selection);

    if (track == nullptr)
        return nullptr;

    auto* undoManager = &edit.getUndoManager();
    track->setName (saved[te::IDs::name].toString());

    if (saved.hasProperty (te::IDs::colour))
        track->state.setProperty (te::IDs::colour, saved[te::IDs::colour], undoManager);

    for (const auto* property : { &schema::phaziteDrumMode, &schema::phaziteDrumOrder, &schema::phaziteDrumHidden }) // view state
        if (saved.hasProperty (*property))
            track->state.setProperty (*property, saved[*property], nullptr);

    // Its modulators and macros, before the devices they modulate.
    if (auto* modulators = track->getModifierList())
        for (const auto& modulator : saved.getChildWithName (te::IDs::MODIFIERS))
            modulators->insertModifier (modulator.createCopy(), -1, nullptr);

    for (const auto& macro : saved.getChildWithName (te::IDs::MACROPARAMETERS))
        track->getMacroParameterListForWriting().state.appendChild (macro.createCopy(), undoManager);

    // Its volume and pan, and its devices in their order.
    for (const auto& device : saved)
    {
        if (! device.hasType (te::IDs::PLUGIN))
            continue;

        if (device[te::IDs::type].toString() == te::VolumeAndPanPlugin::xmlTypeName)
            applyVolume (*track, device);
        else
            insertDevice (*track, edit.getPluginCache().createNewPlugin (device.createCopy()));
    }

    return track;
}

//==============================================================================
bool isPresetFile (const juce::File& file)
{
    return file.hasFileExtension (presetExtension);
}

std::optional<PresetInfo> readPresetInfo (const juce::File& file)
{
    if (! isPresetFile (file) || ! file.existsAsFile())
        return {};

    // Its first element's attributes only.
    juce::XmlDocument document (file);
    const auto root = document.getDocumentElement (true);

    if (root == nullptr || ! root->hasTagName (schema::PHAZITEPRESET.toString()))
        return {};

    const auto kind = findKind (root->getStringAttribute (schema::presetKind.toString()));

    if (! kind.has_value() || root->getIntAttribute (schema::schemaVersion.toString(), schema::currentVersion + 1) > schema::currentVersion)
        return {};

    return PresetInfo { *kind, root->getStringAttribute (schema::presetDevice.toString()),
                        root->getBoolAttribute (schema::presetInstrument.toString()) };
}

juce::String getPresetFolderName (const ProjectContext& context, DeviceID id)
{
    auto* edit = getEdit (context);
    const auto device = edit != nullptr && id.isValid() ? te::findPluginForID (*edit, phazite::toTracktion (id)) : nullptr;

    if (device == nullptr)
        return {};

    return isContainer (*device) ? juce::String ("Containers") : juce::File::createLegalFileName (device->getName());
}

bool saveDevicePreset (const ProjectContext& context, DeviceID id, const juce::File& file)
{
    auto* edit = getEdit (context);
    const auto device = edit != nullptr && id.isValid() ? te::findPluginForID (*edit, phazite::toTracktion (id)) : nullptr;
    return device != nullptr && isDevice (*device) && writePreset (makeDevicePreset (*device), file);
}

bool saveTrackPreset (const ProjectContext& context, TrackID id, const juce::File& file)
{
    auto* edit = getEdit (context);
    auto* track =
        edit != nullptr && id.isValid() ? dynamic_cast<te::AudioTrack*> (te::findTrackForID (*edit, phazite::toTracktion (id))) : nullptr;
    return track != nullptr && getReturnBus (*track) < 0 && writePreset (makeTrackPreset (*track), file);
}

bool loadPreset (ProjectContext& context, const juce::File& file, TrackID target)
{
    auto* edit = getEdit (context);
    auto* track = edit != nullptr && target.isValid() ? te::findTrackForID (*edit, phazite::toTracktion (target)) : nullptr;
    const auto preset = edit != nullptr ? readPreset (file) : juce::ValueTree();

    if (! preset.isValid() || (target.isValid() && track == nullptr))
        return false;

    const auto name = file.getFileNameWithoutExtension();
    const UndoStep step (*edit, "Add " + name);
    auto& selection = getSelection (context);

    if (findKind (preset[schema::presetKind]) == PresetKind::track)
        return addTrackFromPreset (*edit, track, preset, &selection) != nullptr;

    // A container is named after its preset.
    auto* device = addDeviceFromPreset (*edit, track, preset, &selection);

    if (auto* rack = device != nullptr ? getContainerRack (*device) : nullptr)
        rack->rackName = name;

    return device != nullptr;
}

//==============================================================================
namespace
{
    // Whether a preset's device is of a device's kind: the same type, and for a plug-in, the same plug-in.
    bool isSameKindOfDevice (const te::Plugin& device, const juce::ValueTree& saved)
    {
        if (saved[te::IDs::type].toString() != device.state[te::IDs::type].toString())
            return false;

        if (dynamic_cast<const te::ExternalPlugin*> (&device) == nullptr)
            return true;

        for (const auto& property : { te::IDs::uniqueId, te::IDs::name, te::IDs::manufacturer })
            if (saved[property].toString() != device.state[property].toString())
                return false;

        return true;
    }

    // A plug-in's own settings as they are now, as Tracktion writes them when it saves (ExternalPlugin's
    // flushPluginStateToValueTree(), which writes through the undo manager): its state block and its program.
    juce::ValueTree getSettingsNow (te::ExternalPlugin& plugin)
    {
        auto settings = plugin.state.createCopy();

        if (auto* instance = plugin.getAudioPluginInstance())
        {
            juce::MemoryBlock block;
            instance->suspendProcessing (true);
            instance->getStateInformation (block);
            instance->suspendProcessing (false);

            if (block.getSize() > 0)
                settings.setProperty (te::IDs::state, block.toBase64Encoding(), nullptr);

            if (instance->getNumPrograms() > 0)
                settings.setProperty (te::IDs::programNum, instance->getCurrentProgram(), nullptr);
        }

        return settings;
    }

    // A plug-in's own settings, which Tracktion keeps as a block that the plug-in reads only when it loads: put into the
    // plug-in, and back by undo. Neither writes through the undo manager, which an undoable action mustn't.
    class PluginSettingsAction : public juce::UndoableAction
    {
    public:
        PluginSettingsAction (te::ExternalPlugin& plugin, const juce::ValueTree& newSettings)
            : edit (plugin.edit),
              id (plugin.itemID),
              before (getSettingsNow (plugin)),
              after (newSettings.createCopy())
        {
        }

        bool perform() override { return apply (after); }
        bool undo() override { return apply (before); }

    private:
        te::Edit& edit;
        const te::EditItemID id;
        const juce::ValueTree before, after;

        bool apply (const juce::ValueTree& settings)
        {
            const auto found = te::findPluginForID (edit, id);
            auto* plugin = dynamic_cast<te::ExternalPlugin*> (found.get());

            if (plugin == nullptr)
                return false;

            plugin->restorePluginStateFromValueTree (settings);

            // What a save would write, so that the project holds it at once.
            for (const auto& property : { te::IDs::state, te::IDs::programNum })
            {
                if (settings.hasProperty (property))
                    plugin->state.setProperty (property, settings[property], nullptr);
                else
                    plugin->state.removeProperty (property, nullptr);
            }

            return true;
        }
    };

    // One of a plug-in's levels, its dry or its wet, set to what a preset holds, through the undo manager.
    void loadLevel (te::AutomatableParameter& level, const juce::CachedValue<float>& stored, const juce::ValueTree& saved)
    {
        level.setParameter ((float) saved.getProperty (stored.getPropertyID(), stored.getDefault()), juce::sendNotification);
    }
} // namespace

bool loadPresetInto (ProjectContext& context, DeviceID id, const juce::File& file)
{
    auto* edit = getEdit (context);
    const auto device = edit != nullptr && id.isValid() ? te::findPluginForID (*edit, toTracktion (id)) : te::Plugin::Ptr();
    const auto preset = device != nullptr && opensWindow (*device) ? readPreset (file) : juce::ValueTree();

    if (! preset.isValid() || findKind (preset[schema::presetKind]) != PresetKind::device)
        return false;

    const auto saved = preset.getChildWithName (te::IDs::PLUGIN);

    if (! saved.isValid() || ! isSameKindOfDevice (*device, saved))
        return false;

    const UndoStep step (*edit, "Load Preset " + file.getFileNameWithoutExtension());
    auto* plugin = dynamic_cast<te::ExternalPlugin*> (device.get());

    // Tracktion's built-in devices keep their settings and mix as values, which they write through the undo manager.
    if (plugin == nullptr)
    {
        device->restorePluginStateFromValueTree (saved);
        return true;
    }

    // A plug-in keeps its settings in a block of its own, and its mix as values.
    if (! edit->getUndoManager().perform (new PluginSettingsAction (*plugin, saved)))
        return false;

    loadLevel (*plugin->dryGain, plugin->dryValue, saved);
    loadLevel (*plugin->wetGain, plugin->wetValue, saved);
    return true;
}
} // namespace phazite
