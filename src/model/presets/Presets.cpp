// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Presets.h"
#include "PresetModel.h"
#include "devices/drums/DrumSampler.h"
#include "devices/wavetable/WavetableSynth.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DeviceFiles.h"
#include "model/devices/internal/DevicePlugin.h"
#include "model/devices/internal/DeviceStates.h"
#include "model/devices/internal/Devices.h"
#include "model/mirror/internal/Bridge.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

#include <array>
#include <charconv>
#include <functional>
#include <iterator>

namespace phazite
{
namespace
{
    namespace keys = schema::preset;

    std::optional<PresetKind> findKind (const juce::String& name)
    {
        if (name == keys::deviceKind)
            return PresetKind::device;

        if (name == keys::containerKind)
            return PresetKind::container;

        if (name == keys::trackKind)
            return PresetKind::track;

        return {};
    }

    juce::var makeObject()
    {
        return juce::var (new juce::DynamicObject());
    }

    void set (const juce::var& object, const juce::Identifier& key, const juce::var& value)
    {
        if (auto* properties = object.getDynamicObject())
            properties->setProperty (key, value);
    }

    juce::String toString (std::string_view text)
    {
        return juce::String (text.data(), text.size());
    }

    //==============================================================================
    // Phazite's own devices, by their type IDs.
    const devices::DeviceInfo* findOwnDevice (const juce::String& typeID)
    {
        for (const auto* info : getOwnDevices())
            if (toString (info->typeID) == typeID)
                return info;

        return nullptr;
    }

    // A parameter's value as a preset writes it: a choice's or a switch's as a whole number, any other as the shortest
    // number that reads back as the same float ("0.6", rather than "0.6000000238418579", every digit of it as a double).
    juce::var toPresetValue (const devices::ParameterInfo& parameter, float value)
    {
        if (parameter.unit == devices::Unit::choice || parameter.unit == devices::Unit::toggle)
            return juce::roundToInt (value);

        std::array<char, 32> text {};
        const auto [end, error] = std::to_chars (text.data(), text.data() + text.size(), value);
        return error == std::errc() ? juce::String (text.data(), (size_t) (end - text.data())).getDoubleValue() : (double) value;
    }

    // A plug-in's description, as scanning found it: by its identifier, else by its name, maker and format.
    std::optional<juce::PluginDescription> findPlugin (te::Engine& engine, const juce::var& identity)
    {
        juce::PluginDescription wanted;
        wanted.fileOrIdentifier = identity[keys::identifier].toString();
        wanted.name = identity[keys::name].toString();
        wanted.manufacturerName = identity[keys::manufacturer].toString();
        wanted.pluginFormatName = identity[keys::format].toString();
        return findKnownPlugin (engine, wanted);
    }

    bool isSamePlugin (const te::ExternalPlugin& plugin, const juce::var& identity)
    {
        return plugin.desc.name == identity[keys::name].toString()
               && plugin.desc.manufacturerName == identity[keys::manufacturer].toString()
               && plugin.desc.pluginFormatName == identity[keys::format].toString();
    }

    //==============================================================================
    // A device as a preset holds it: its values set by hand, not what automation or modulation played; its sound files by
    // their full paths; nothing that ties it to its project (its automation, what modulates it, its sidechain, whether it
    // is on).
    juce::var describeDevice (te::Plugin& device)
    {
        storeDeviceState (device);
        auto preset = makeObject();
        set (preset, keys::name, device.getName());
        set (preset, keys::instrument, isInstrument (device));

        if (auto* own = dynamic_cast<DevicePlugin*> (&device))
        {
            auto state = device.state.createCopy();
            restoreValuesSetByHand (state);
            writeDeviceFilePaths (getSong (device.edit), state);
            const auto& info = own->getDeviceInfo();
            set (preset, keys::type, toString (info.typeID));

            const auto parameters = makeObject();

            // Every parameter, so that the preset sets the whole device wherever it loads.
            for (const auto& parameter : info.parameters)
            {
                const juce::Identifier id (toString (parameter.id));
                set (parameters, id, toPresetValue (parameter, state.hasProperty (id) ? (float) state[id] : parameter.defaultValue));
            }

            set (preset, keys::parameters, parameters);
            const auto sounds = makeObject();

            for (const auto& property : getSoundProperties (info))
                if (state.hasProperty (property))
                    set (sounds, property, state[property].toString());

            if (! sounds.getDynamicObject()->getProperties().isEmpty())
                set (preset, keys::sounds, sounds);

            return preset;
        }

        if (auto* plugin = dynamic_cast<te::ExternalPlugin*> (&device))
        {
            const auto identity = makeObject();
            set (identity, keys::format, plugin->desc.pluginFormatName);
            set (identity, keys::name, plugin->desc.name);
            set (identity, keys::manufacturer, plugin->desc.manufacturerName);
            set (identity, keys::identifier, plugin->desc.fileOrIdentifier);
            set (identity, keys::uid, plugin->desc.uniqueId);
            set (preset, keys::plugin, identity);
            set (preset, keys::state, plugin->state[te::IDs::state].toString());
            set (preset, keys::program, (int) plugin->state[te::IDs::programNum]);
            set (preset, keys::mix, getMix (device));
            return preset;
        }

        return {};
    }

    // A modulation's target, as a preset names it (Schema.h); void for one the preset doesn't hold (a send).
    juce::var describeTarget (const te::AutomatableParameter& parameter, float amount,
                              const std::function<bool (const juce::var&, te::Plugin&)>& where)
    {
        auto* owner = parameter.getPlugin();
        auto target = makeObject();

        if (owner == nullptr || ! where (target, *owner))
            return {};

        set (target, keys::parameter, parameter.paramID);
        set (target, keys::amount, amount);
        return target;
    }

    juce::Array<juce::var> describeModulations (const ModulatorAccess& modulator,
                                                const std::function<bool (const juce::var&, te::Plugin&)>& where)
    {
        juce::Array<juce::var> targets;

        for (const auto& modulation : getModulations (modulator))
            if (const auto target = describeTarget (*modulation.parameter, modulation.assignment->value.get(), where); target.isObject())
                targets.add (target);

        return targets;
    }

    juce::var describeMacro (const ModulatorAccess& macro, const std::function<bool (const juce::var&, te::Plugin&)>& where)
    {
        auto preset = makeObject();
        set (preset, keys::name, macro.macro->macroName.get());
        set (preset, keys::value, macro.macro->getCurrentBaseValue());
        set (preset, keys::modulates, describeModulations (macro, where));
        return preset;
    }

    // A container: its devices, and its macros with what they modulate in it.
    juce::var describeContainer (te::Plugin& container)
    {
        auto* rack = getContainerRack (container);
        auto preset = makeObject();

        if (rack == nullptr)
            return {};

        storeDeviceStates (*rack);
        set (preset, keys::name, rack->rackName.get());
        set (preset, keys::mix, getMix (container));

        const auto inside = getContainerDevices (container);
        juce::Array<juce::var> devices, macros;

        for (auto* device : inside)
            devices.add (describeDevice (*device));

        const auto inContainer = [&inside] (const juce::var& target, te::Plugin& owner)
        {
            if (const auto index = inside.indexOf (&owner); index >= 0)
            {
                set (target, keys::device, index);
                return true;
            }

            return false;
        };

        for (auto* macro : getContainerMacros (container))
            macros.add (describeMacro ({ nullptr, macro }, inContainer));

        set (preset, keys::devices, devices);
        set (preset, keys::macros, macros);
        return preset;
    }

    //==============================================================================
    // A device made from a preset, on no channel yet: one of Phazite's own with its values and sounds (the sounds copied
    // into the project), or a plug-in with its own settings; nullptr for a plug-in that isn't installed.
    te::Plugin::Ptr createDevice (te::Edit& edit, const juce::var& preset)
    {
        if (const auto* info = findOwnDevice (preset[keys::type].toString()))
        {
            juce::ValueTree state (te::IDs::PLUGIN);
            state.setProperty (te::IDs::type, toString (info->typeID), nullptr);
            const auto values = preset[keys::parameters];

            for (const auto& parameter : info->parameters)
                if (const juce::Identifier id (toString (parameter.id)); values.hasProperty (id))
                    state.setProperty (id, devices::constrain (parameter, (float) values[id]), nullptr);

            for (const auto& property : getSoundProperties (*info))
                if (const auto sound = preset[keys::sounds][property].toString(); sound.isNotEmpty())
                    state.setProperty (property, sound, nullptr);

            copyDeviceFilesInto (getSong (edit), state);
            return edit.getPluginCache().createNewPlugin (state);
        }

        if (const auto identity = preset[keys::plugin]; identity.isObject())
        {
            const auto description = findPlugin (edit.engine, identity);

            if (! description.has_value())
                return {};

            auto state = te::ExternalPlugin::create (edit.engine, *description);

            if (const auto settings = preset[keys::state].toString(); settings.isNotEmpty())
                state.setProperty (te::IDs::state, settings, nullptr);

            state.setProperty (te::IDs::programNum, (int) preset[keys::program], nullptr);
            return edit.getPluginCache().createNewPlugin (state);
        }

        return {};
    }

    // What a preset sets once its device is on a channel: a plug-in's mix.
    void applyMix (te::Plugin& device, const juce::var& preset)
    {
        if (preset.hasProperty (keys::mix) && preset[keys::plugin].isObject())
            setMix (device, (float) preset[keys::mix]);
    }

    // A parameter of a device by its ID, or nullptr.
    te::AutomatableParameter* findParameter (te::Plugin* device, const juce::String& id)
    {
        return device != nullptr && id.isNotEmpty() ? device->getAutomatableParameterByID (id).get() : nullptr;
    }

    // A macro's name and value, from a preset.
    void applyMacro (const ModulatorAccess& macro, const juce::var& preset)
    {
        if (const auto name = preset[keys::name].toString(); name.isNotEmpty())
            renameMacro (*macro.macro, name);

        setDeviceValue (*macro.macro, juce::jlimit (0.0f, 1.0f, (float) preset[keys::value]));
    }

    // What a modulator or a macro modulates, from a preset; find() gives a target's parameter.
    void applyModulations (const ModulatorAccess& modulator, const juce::var& preset,
                           const std::function<te::AutomatableParameter*(const juce::var&)>& find)
    {
        if (const auto* targets = preset[keys::modulates].getArray())
            for (const auto& target : *targets)
                if (auto* parameter = find (target); parameter != nullptr && canModulate (modulator, *parameter))
                    setModulation (modulator, *parameter, juce::jlimit (-1.0f, 1.0f, (float) target[keys::amount]));
    }

    // A container made from a preset and put on a channel: its devices, mix, name and macros, with what they modulate.
    te::Plugin* addContainer (te::Edit& edit, te::Track* channel, const juce::var& preset,
                              const std::function<te::Plugin*(te::Edit&, te::Track*, const te::Plugin::Ptr&)>& place)
    {
        te::Plugin::Array made;
        juce::Array<juce::var> madeFrom;

        if (const auto* devices = preset[keys::devices].getArray())
            for (const auto& device : *devices)
                if (auto plugin = createDevice (edit, device))
                {
                    made.add (plugin);
                    madeFrom.add (device);
                }

        auto* container = place (edit, channel, createContainer (edit, made, preset[keys::name].toString()));

        if (container == nullptr)
            return nullptr;

        setMix (*container, preset.hasProperty (keys::mix) ? (float) preset[keys::mix] : 1.0f);
        const auto inside = getContainerDevices (*container);

        for (int i = 0; i < inside.size() && i < madeFrom.size(); ++i)
            applyMix (*inside[i], madeFrom[i]);

        const auto find = [&inside] (const juce::var& target)
        {
            const auto index = (int) target.getProperty (keys::device, -1);
            return juce::isPositiveAndBelow (index, inside.size()) ? findParameter (inside[index], target[keys::parameter].toString())
                                                                   : nullptr;
        };

        const auto macros = getContainerMacros (*container);

        if (const auto* saved = preset[keys::macros].getArray())
        {
            for (int i = 0; i < macros.size() && i < saved->size(); ++i)
            {
                const ModulatorAccess macro { nullptr, macros[i] };
                applyMacro (macro, saved->getReference (i));
                applyModulations (macro, saved->getReference (i), find);
            }
        }

        return container;
    }

    //==============================================================================
    // A track's volume and pan as a preset keeps them: in decibels, and from -1 to 1.
    void describeMix (const juce::var& preset, te::AudioTrack& track)
    {
        set (preset, keys::volume, getChannelVolume (track));
        set (preset, keys::pan, getChannelPan (track));
    }

    void applyMix (te::AudioTrack& track, const juce::var& preset)
    {
        if (preset.hasProperty (keys::volume))
            setChannelVolume (track, (float) preset[keys::volume]);

        if (preset.hasProperty (keys::pan))
            setChannelPan (track, (float) preset[keys::pan]);
    }

} // namespace

//==============================================================================
juce::var makeDevicePreset (te::Plugin& device)
{
    auto preset = makeObject();

    if (getContainerRack (device) != nullptr)
    {
        set (preset, keys::kind, keys::containerKind);
        set (preset, keys::container, describeContainer (device));
        return preset;
    }

    const auto described = describeDevice (device);

    if (! described.isObject())
        return {};

    set (preset, keys::kind, keys::deviceKind);
    set (preset, keys::device, described);
    return preset;
}

juce::var makeTrackPreset (te::AudioTrack& track)
{
    storeDeviceStates (track);
    auto saved = makeObject();
    const auto state = findTrackState (track);
    set (saved, keys::name, getTrackName (track));
    set (saved, keys::colour, juce::Colour::fromString (state[schema::trackColour].toString()).toString());

    for (const auto& [property, key] :
         { std::pair { &schema::drumMode, &keys::drumMode }, std::pair { &schema::drumOrder, &keys::drumOrder },
           std::pair { &schema::drumHidden, &keys::drumHidden } })
        if (state.hasProperty (*property))
            set (saved, *key, state[*property]);

    describeMix (saved, track);

    // Its instrument, and its effects and containers, in their order.
    auto* instrument = getInstrument (track);
    const auto effects = getEffects (track);

    if (instrument != nullptr)
        set (saved, keys::instrument, describeDevice (*instrument));

    juce::Array<juce::var> effectPresets;

    for (auto* effect : effects)
    {
        if (getContainerRack (*effect) != nullptr)
        {
            auto container = makeObject();
            set (container, keys::container, describeContainer (*effect));
            effectPresets.add (container);
        }
        else
        {
            effectPresets.add (describeDevice (*effect));
        }
    }

    set (saved, keys::effects, effectPresets);

    // Its modulators and macros, with what they modulate on it: its devices', those in its containers, its volume and pan.
    const auto onTrack = [&track, instrument, &effects] (const juce::var& target, te::Plugin& owner)
    {
        if (&owner == track.getVolumePlugin())
        {
            set (target, keys::mixer, true);
            return true;
        }

        if (&owner == instrument)
        {
            set (target, keys::instrument, true);
            return true;
        }

        for (int i = 0; i < effects.size(); ++i)
        {
            if (effects[i] == &owner)
            {
                set (target, keys::effect, i);
                return true;
            }

            if (const auto inside = getContainerDevices (*effects[i]).indexOf (&owner);
                getContainerRack (*effects[i]) != nullptr && inside >= 0)
            {
                set (target, keys::effect, i);
                set (target, keys::inside, inside);
                return true;
            }
        }

        return false; // a send, which the preset doesn't hold
    };

    juce::Array<juce::var> modulators, macros;

    for (const auto& modulator : getModulators (track))
    {
        const auto type = getModulatorType (modulator);

        if (! type.has_value())
            continue;

        if (*type == ModulatorType::macro)
        {
            macros.add (describeMacro (modulator, onTrack));
            continue;
        }

        auto preset = makeObject();
        set (preset, keys::type, keys::modulatorTypeNames[(size_t) *type]);
        const auto parameters = makeObject();

        for (auto* parameter : modulator.modifier->getAutomatableParameters())
            if (parameter != modulator.modifier->enabledParam.get()) // presets load modulators on
                set (parameters, parameter->paramID, parameter->getCurrentBaseValue());

        set (preset, keys::parameters, parameters);

        if (auto* sequencer = dynamic_cast<te::StepModifier*> (modulator.modifier.get()))
        {
            juce::Array<juce::var> steps;

            for (int i = 0; i < juce::roundToInt (sequencer->numStepsParam->getCurrentBaseValue()); ++i)
                steps.add (sequencer->getStep (i));

            set (preset, keys::steps, steps);
        }

        set (preset, keys::modulates, describeModulations (modulator, onTrack));
        modulators.add (preset);
    }

    set (saved, keys::modulators, modulators);
    set (saved, keys::macros, macros);

    auto preset = makeObject();
    set (preset, keys::kind, keys::trackKind);
    set (preset, keys::track, saved);
    return preset;
}

juce::var readPreset (const juce::File& file)
{
    if (! file.existsAsFile())
        return {};

    const auto preset = juce::JSON::parse (file);

    if (! preset.isObject() || ! findKind (preset[keys::kind].toString()).has_value())
        return {};

    return preset;
}

bool writePreset (const juce::var& preset, const juce::File& file)
{
    return preset.isObject() && file.getParentDirectory().createDirectory()
           && file.replaceWithText (juce::JSON::toString (preset, false) + "\n", false, false, "\n");
}

te::Plugin* addDeviceFromPreset (te::Edit& edit, te::Track* channel, const juce::var& preset, Selection* selection)
{
    const auto place = [selection] (te::Edit& in, te::Track* on, const te::Plugin::Ptr& device)
    { return device != nullptr ? placeDevice (in, on, device, selection) : nullptr; };

    if (findKind (preset[keys::kind].toString()) == PresetKind::container)
        return addContainer (edit, channel, preset[keys::container], place);

    const auto saved = preset[keys::device];
    auto* device = place (edit, channel, createDevice (edit, saved));

    if (device != nullptr)
        applyMix (*device, saved);

    return device;
}

te::AudioTrack* addTrackFromPreset (te::Edit& edit, te::Track* after, const juce::var& preset, Selection* selection)
{
    const auto saved = preset[keys::track];

    if (! saved.isObject())
        return nullptr;

    auto* track = insertTrack (edit, after, selection);

    if (track == nullptr)
        return nullptr;

    setTrackName (*track, saved[keys::name].toString());

    if (const auto colour = saved[keys::colour].toString(); colour.isNotEmpty())
        setTrackColour (*track, juce::Colour::fromString (colour));

    for (const auto& [property, key] :
         { std::pair { &schema::drumMode, &keys::drumMode }, std::pair { &schema::drumOrder, &keys::drumOrder },
           std::pair { &schema::drumHidden, &keys::drumHidden } })
        if (saved.hasProperty (*key))
            findTrackState (*track).setProperty (*property, saved[*key], nullptr); // view state

    applyMix (*track, saved);

    // Its instrument, then its effects and containers in their order.
    te::Plugin* instrument = nullptr;
    te::Plugin::Array effects;

    if (const auto device = saved[keys::instrument]; device.isObject())
        if ((instrument = setInstrument (*track, createDevice (edit, device))) != nullptr)
            applyMix (*instrument, device);

    const auto insert = [] (te::Edit&, te::Track* on, const te::Plugin::Ptr& device)
    { return device != nullptr && on != nullptr ? insertDevice (*on, device) : nullptr; };

    if (const auto* effectPresets = saved[keys::effects].getArray())
    {
        for (const auto& effect : *effectPresets)
        {
            te::Plugin* added = nullptr;

            if (const auto container = effect[keys::container]; container.isObject())
                added = addContainer (edit, track, container, insert);
            else if ((added = insert (edit, track, createDevice (edit, effect))) != nullptr)
                applyMix (*added, effect);

            effects.add (added); // nullptr for one that couldn't be made, so that the others keep their indices
        }
    }

    // Its modulators and macros, then what they modulate.
    const auto find = [track, instrument, &effects] (const juce::var& target) -> te::AutomatableParameter*
    {
        const auto id = target[keys::parameter].toString();

        if ((bool) target[keys::mixer])
        {
            auto* volume = track->getVolumePlugin();
            return volume == nullptr ? nullptr : id == "pan" ? volume->panParam.get() : id == "volume" ? volume->volParam.get() : nullptr;
        }

        if ((bool) target[keys::instrument])
            return findParameter (instrument, id);

        const auto effect = (int) target.getProperty (keys::effect, -1);
        te::Plugin* device = juce::isPositiveAndBelow (effect, effects.size()) ? effects[effect].get() : nullptr;

        if (device != nullptr && target.hasProperty (keys::inside))
        {
            const auto inside = getContainerDevices (*device);
            const auto index = (int) target[keys::inside];
            device = juce::isPositiveAndBelow (index, inside.size()) ? inside[index] : nullptr;
        }

        return findParameter (device, id);
    };

    if (const auto* modulators = saved[keys::modulators].getArray())
    {
        for (const auto& modulator : *modulators)
        {
            const auto type = findModulatorType (modulator[keys::type].toString());
            const auto added = type.has_value() ? addModulator (*track, *type) : std::nullopt;

            if (! added.has_value() || added->modifier == nullptr)
                continue;

            if (auto* values = modulator[keys::parameters].getDynamicObject())
                for (const auto& [id, value] : values->getProperties())
                    if (auto parameter = added->modifier->getAutomatableParameterByID (id.toString()); parameter != nullptr)
                        setDeviceValue (*parameter, (float) value);

            if (const auto* steps = modulator[keys::steps].getArray())
                for (int i = 0; i < steps->size(); ++i)
                    setModulatorStep (*added, i, (float) steps->getReference (i));

            applyModulations (*added, modulator, find);
        }
    }

    if (const auto* macros = saved[keys::macros].getArray())
    {
        for (const auto& macro : *macros)
        {
            if (auto* made = addMacro (track->getMacroParameterListForWriting(), macro[keys::name].toString()))
            {
                const ModulatorAccess access { nullptr, made };
                applyMacro (access, macro);
                applyModulations (access, macro, find);
            }
        }
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
    if (! isPresetFile (file))
        return {};

    const auto preset = readPreset (file);
    const auto kind = findKind (preset[keys::kind].toString());

    if (! kind.has_value())
        return {};

    const auto device = preset[keys::device];
    return PresetInfo { *kind, device[keys::name].toString(), (bool) device[keys::instrument] };
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
    return device != nullptr && findDeviceState (*edit, id).isValid() && writePreset (makeDevicePreset (*device), file);
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
    const auto preset = edit != nullptr ? readPreset (file) : juce::var();

    if (! preset.isObject() || (target.isValid() && track == nullptr))
        return false;

    const auto name = file.getFileNameWithoutExtension();
    const UndoStep step (*edit, "Add " + name);
    auto& selection = getSelection (context);

    if (findKind (preset[keys::kind].toString()) == PresetKind::track)
        return addTrackFromPreset (*edit, track, preset, &selection) != nullptr;

    // A container is named after its preset.
    auto* device = addDeviceFromPreset (*edit, track, preset, &selection);

    if (device != nullptr && isContainer (*device))
        setContainerName (*device, name);

    return device != nullptr;
}

//==============================================================================
namespace
{
    // A plug-in's state with its own settings as they are now (its state block and its program), which this stores in the
    // project first, outside the undo history.
    juce::ValueTree getSettingsNow (te::ExternalPlugin& plugin)
    {
        storeDeviceState (plugin);
        return plugin.state.createCopy();
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

            // What a save would write, so that the project holds it at once: in the plug-in's state, and in the song's
            // element of it (Schema.h: pluginState, pluginProgram), but for a plug-in in a container.
            auto device = findDeviceState (edit, getDeviceID (*plugin));

            for (const auto& [property, inSong] :
                 { std::pair { te::IDs::state, schema::pluginState }, std::pair { te::IDs::programNum, schema::pluginProgram } })
            {
                if (settings.hasProperty (property))
                    plugin->state.setProperty (property, settings[property], nullptr);
                else
                    plugin->state.removeProperty (property, nullptr);

                if (device.isValid() && settings[property].toString().isNotEmpty())
                    device.setProperty (inSong, settings[property], nullptr);
                else if (device.isValid())
                    device.removeProperty (inSong, nullptr);
            }

            return true;
        }
    };

    // A preset's values put into one of Phazite's own devices of its type, each parameter through the undo manager, and a
    // parameter it doesn't name at its default, so that a preset is the same starting point whatever the device was set to;
    // its sounds too, copied into the project, and the device's own sound where it names none.
    bool loadInto (DevicePlugin& device, const juce::var& saved)
    {
        const auto& info = device.getDeviceInfo();

        if (saved[keys::type].toString() != toString (info.typeID))
            return false;

        const auto values = saved[keys::parameters];

        for (const auto& parameter : info.parameters)
        {
            const juce::Identifier id (toString (parameter.id));

            if (auto* found = findParameter (&device, id.toString()))
            {
                const auto value = values.hasProperty (id) ? (float) values[id] : parameter.defaultValue;
                setDeviceValue (*found, devices::constrain (parameter, value));
            }
        }

        juce::ValueTree sounds (te::IDs::PLUGIN);
        sounds.setProperty (te::IDs::type, toString (info.typeID), nullptr);

        for (const auto& property : getSoundProperties (info))
            if (const auto sound = saved[keys::sounds][property].toString(); sound.isNotEmpty())
                sounds.setProperty (property, sound, nullptr);

        copyDeviceFilesInto (getSong (device.edit), sounds);

        for (const auto& property : getSoundProperties (info))
            if (sounds.hasProperty (property))
                setDeviceSound (device, property, sounds[property].toString());
            else if (! saved[keys::sounds].hasProperty (property))
                setDeviceSound (device, property, {}); // its own sound again

        return true;
    }
} // namespace

bool loadPresetInto (ProjectContext& context, DeviceID id, const juce::File& file)
{
    auto* edit = getEdit (context);
    const auto device = edit != nullptr && id.isValid() ? te::findPluginForID (*edit, toTracktion (id)) : te::Plugin::Ptr();
    const auto preset = device != nullptr ? readPreset (file) : juce::var();

    if (! preset.isObject() || findKind (preset[keys::kind].toString()) != PresetKind::device)
        return false;

    const auto saved = preset[keys::device];

    if (auto* own = dynamic_cast<DevicePlugin*> (device.get()))
    {
        if (saved[keys::type].toString() != toString (own->getDeviceInfo().typeID))
            return false;

        const UndoStep step (*edit, "Load Preset " + file.getFileNameWithoutExtension());
        return loadInto (*own, saved);
    }

    auto* plugin = dynamic_cast<te::ExternalPlugin*> (device.get());

    if (plugin == nullptr || ! saved[keys::plugin].isObject() || ! isSamePlugin (*plugin, saved[keys::plugin]))
        return false;

    // A plug-in keeps its settings in a block of its own, and its mix as values.
    juce::ValueTree settings (te::IDs::PLUGIN);
    settings.setProperty (te::IDs::state, saved[keys::state].toString(), nullptr);
    settings.setProperty (te::IDs::programNum, (int) saved[keys::program], nullptr);
    const UndoStep step (*edit, "Load Preset " + file.getFileNameWithoutExtension());

    if (! getSong (*edit).getUndoManager().perform (new PluginSettingsAction (*plugin, settings)))
        return false;

    applyMix (*plugin, saved);
    return true;
}
} // namespace phazite
