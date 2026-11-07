// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Presets.h"
#include "PresetModel.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DeviceFiles.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginHost.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <functional>
#include <optional>
#include <vector>

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
    // The description of one of Phazite's own devices an element names, or nullptr (a plug-in, a container).
    const devices::DeviceInfo* findOwnInfo (const juce::ValueTree& device)
    {
        return device.hasType (schema::DEVICE) && ! isPluginState (device)
                   ? findOwnDevice (device[schema::deviceType].toString().toStdString())
                   : nullptr;
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

    // The plug-in a preset names (Schema.h: preset::plugin), which the plug-in host finds among those scanning found: by its
    // identifier, else by its name, maker and format.
    juce::PluginDescription readIdentity (const juce::var& identity)
    {
        juce::PluginDescription wanted;
        wanted.fileOrIdentifier = identity[keys::identifier].toString();
        wanted.name = identity[keys::name].toString();
        wanted.manufacturerName = identity[keys::manufacturer].toString();
        wanted.pluginFormatName = identity[keys::format].toString();
        return wanted;
    }

    bool isSamePlugin (const juce::ValueTree& device, const juce::var& identity)
    {
        const auto description = getPluginIdentity (device);
        return description.name == identity[keys::name].toString()
               && description.manufacturerName == identity[keys::manufacturer].toString()
               && description.pluginFormatName == identity[keys::format].toString();
    }

    // How many steps a step sequencer's element plays (its numSteps set by hand, else its default).
    int getNumSteps (const juce::ValueTree& sequencer)
    {
        constexpr auto kind = devices::ModulatorKind::steps;
        const auto& info = devices::getModulatorParameters (kind)[(size_t) devices::findModulatorParameter (kind, "numSteps")];
        const auto value =
            (float) sequencer.getChildWithName (schema::VALUES).getProperty (juce::Identifier (toString (info.id)), info.defaultValue);
        return juce::jlimit (1, devices::Modulator::maxSteps, juce::roundToInt (value));
    }

    //==============================================================================
    // A device as a preset holds it: its values set by hand, not what automation or modulation played; its sound files by
    // their full paths; nothing that ties it to its project (its automation, what modulates it, its sidechain, whether it
    // is on). Void for an element that is neither one of Phazite's own devices nor a plug-in.
    juce::var describeDevice (const Song& song, const juce::ValueTree& device)
    {
        const auto id = getDeviceID (device);
        const auto* own = findOwnInfo (device);

        if (own == nullptr && ! isPluginState (device))
            return {};

        auto preset = makeObject();
        set (preset, keys::name, getDeviceName (song, id));
        set (preset, keys::instrument, isInstrumentState (device));

        if (own != nullptr)
        {
            set (preset, keys::type, toString (own->typeID));

            // Every parameter, so that the preset sets the whole device wherever it loads.
            const auto values = device.getChildWithName (schema::VALUES);
            const auto parameters = makeObject();

            for (const auto& parameter : own->parameters)
            {
                const juce::Identifier parameterID (toString (parameter.id));
                set (parameters, parameterID, toPresetValue (parameter, (float) values.getProperty (parameterID, parameter.defaultValue)));
            }

            set (preset, keys::parameters, parameters);

            // Its sounds, its files by their full paths.
            auto copy = device.createCopy();
            writeDeviceFilePaths (song, copy);
            const auto sounds = copy.getChildWithName (schema::SOUNDS);
            const auto saved = makeObject();

            for (const auto& property : getSoundProperties (*own))
                if (sounds.hasProperty (property))
                    set (saved, property, sounds[property].toString());

            if (! saved.getDynamicObject()->getProperties().isEmpty())
                set (preset, keys::sounds, saved);

            return preset;
        }

        // A plug-in: which one it is, and what it keeps of its own (its state and its program, stored into its element as
        // the caller stored the song's devices), and its mix.
        const auto description = getPluginIdentity (device);
        const auto identity = makeObject();
        set (identity, keys::format, description.pluginFormatName);
        set (identity, keys::name, description.name);
        set (identity, keys::manufacturer, description.manufacturerName);
        set (identity, keys::identifier, description.fileOrIdentifier);
        set (identity, keys::uid, description.uniqueId);
        set (preset, keys::plugin, identity);
        set (preset, keys::state, device[schema::pluginState].toString());
        set (preset, keys::program, (int) device[schema::pluginProgram]);
        set (preset, keys::mix, getMix (song, id));
        return preset;
    }

    // Where a modulation's target is, as a preset names it (Schema.h: TARGET), written into the target; false for one the
    // preset doesn't hold (a send).
    using Locate = std::function<bool (const juce::var& target, const ParameterRef&)>;

    // What a modulator or a macro modulates, from its element (Schema.h: MODULATES), in the order it was made.
    juce::Array<juce::var> describeModulations (const juce::ValueTree& modulator, const Locate& where)
    {
        juce::Array<juce::var> targets;

        for (const auto& child : modulator)
        {
            if (! child.hasType (schema::MODULATES))
                continue;

            const auto parameter = getModulatedParameter (child);
            auto target = makeObject();

            if (! where (target, parameter))
                continue;

            set (target, keys::parameter, parameter.parameter);
            set (target, keys::amount, (float) child[schema::modulatesAmount]);
            targets.add (target);
        }

        return targets;
    }

    juce::var describeMacro (const juce::ValueTree& macro, const Locate& where)
    {
        auto preset = makeObject();
        set (preset, keys::name, macro[schema::macroName].toString());
        set (preset, keys::value, (float) macro.getProperty (schema::macroValue, 0.0f));
        set (preset, keys::modulates, describeModulations (macro, where));
        return preset;
    }

    // A container: its devices, and its macros with what they modulate in it.
    juce::var describeContainer (const Song& song, const juce::ValueTree& container)
    {
        auto preset = makeObject();
        set (preset, keys::name, container[schema::containerName].toString());
        set (preset, keys::mix, getMix (song, getDeviceID (container)));

        const auto inside = getDeviceStates (container);
        juce::Array<juce::var> devicePresets, macroPresets;

        for (const auto& device : inside)
            devicePresets.add (describeDevice (song, device));

        const auto inContainer = [&inside] (const juce::var& target, const ParameterRef& parameter)
        {
            for (size_t i = 0; i < inside.size(); ++i)
            {
                if (getDeviceID (inside[i]) == parameter.device)
                {
                    set (target, keys::device, (int) i);
                    return true;
                }
            }

            return false;
        };

        for (const auto& child : container)
            if (child.hasType (schema::MACRO))
                macroPresets.add (describeMacro (child, inContainer));

        set (preset, keys::devices, devicePresets);
        set (preset, keys::macros, macroPresets);
        return preset;
    }

    //==============================================================================
    // One of Phazite's own devices' element made from a preset: the values it names, each as its parameter allows it (the
    // others at their defaults), and its sounds, their files copied into the project.
    juce::ValueTree makeOwnDevice (Song& song, const devices::DeviceInfo& info, const juce::var& saved)
    {
        std::vector<std::pair<juce::String, float>> values;
        const auto named = saved[keys::parameters];

        for (const auto& parameter : info.parameters)
            if (const juce::Identifier parameterID (toString (parameter.id)); named.hasProperty (parameterID))
                values.push_back ({ parameterID.toString(), devices::constrain (parameter, (float) named[parameterID]) });

        auto state = makeOwnDeviceState (song, info.typeID, values);
        juce::ValueTree sounds (schema::SOUNDS);

        for (const auto& property : getSoundProperties (info))
            if (const auto sound = saved[keys::sounds][property].toString(); sound.isNotEmpty())
                sounds.setProperty (property, sound, nullptr);

        if (sounds.getNumProperties() > 0)
            state.appendChild (sounds, nullptr);

        copyDeviceFilesInto (song, state);
        return state;
    }

    // A plug-in's element made from a preset: which plug-in it is, as scanning found it, what it keeps of its own (its state
    // and its program, which its instance opens with) and an effect's mix; invalid for one the plug-in host doesn't know.
    juce::ValueTree makePlugin (Song& song, const PluginHost& host, const juce::var& saved)
    {
        const auto description = findKnownPlugin (host, readIdentity (saved[keys::plugin]));

        if (! description.has_value())
            return {};

        DeviceType type;
        type.name = description->name;
        type.description = *description;
        type.isInstrument = description->isInstrument;
        auto state = makeDeviceState (song, type);

        if (const auto settings = saved[keys::state].toString(); settings.isNotEmpty())
            state.setProperty (schema::pluginState, settings, nullptr);

        if (saved.hasProperty (keys::program))
            state.setProperty (schema::pluginProgram, (int) saved[keys::program], nullptr);

        if (const auto mix = juce::jlimit (0.0f, 1.0f, (float) saved.getProperty (keys::mix, 1.0f));
            ! isInstrumentState (state) && mix < 1.0f)
            state.setProperty (schema::deviceMix, mix, nullptr);

        return state;
    }

    // A device preset's device (Schema.h: DEVICE), one of Phazite's own or a plug-in.
    juce::ValueTree makeDevice (Song& song, const PluginHost& host, const juce::var& saved)
    {
        if (const auto* info = findOwnDevice (saved[keys::type].toString().toStdString()))
            return makeOwnDevice (song, *info, saved);

        if (saved[keys::plugin].isObject())
            return makePlugin (song, host, saved);

        return {};
    }

    // Whether a device's element has a parameter a preset names: one of Phazite's own devices' by its description; any for a
    // plug-in, whose instance tells its parameters only once it plays (what names one it hasn't plays nothing).
    bool hasParameter (const juce::ValueTree& device, const juce::String& parameterID)
    {
        if (const auto* info = findOwnInfo (device))
            return devices::findParameter (*info, parameterID.toStdString()) >= 0;

        return isPluginState (device) && parameterID.isNotEmpty();
    }

    // What a macro of an element not in the song yet modulates, as the song keeps it (Modulation.h: setModulation()).
    void addModulates (juce::ValueTree macro, const ParameterRef& parameter, float amount)
    {
        juce::ValueTree modulates (schema::MODULATES);
        modulates.setProperty (schema::modulatesDevice, juce::String ((juce::int64) parameter.device.getRaw()), nullptr);
        modulates.setProperty (schema::modulatesParameter, parameter.parameter, nullptr);
        modulates.setProperty (schema::modulatesAmount, std::round ((double) juce::jlimit (-1.0f, 1.0f, amount) * 1.0e4) / 1.0e4, nullptr);
        macro.appendChild (modulates, nullptr);
    }

    // A container made from a preset (Schema.h: CONTAINER), with the devices it holds, by their places in the preset (none
    // for one that couldn't be made, so that the others keep their places: a plug-in that isn't installed, or an
    // instrument, which a container can't hold).
    struct MadeContainer
    {
        juce::ValueTree state;
        std::vector<DeviceID> devices;
    };

    MadeContainer makeContainer (Song& song, const PluginHost& host, const juce::var& saved)
    {
        MadeContainer made;
        made.state = juce::ValueTree (schema::CONTAINER);
        made.state.setProperty (schema::deviceID, (juce::int64) song.makeID(), nullptr);
        const auto name = saved[keys::name].toString();
        made.state.setProperty (schema::containerName, name.isNotEmpty() ? name : makeContainerName (song), nullptr);

        if (const auto mix = juce::jlimit (0.0f, 1.0f, (float) saved.getProperty (keys::mix, 1.0f)); mix < 1.0f)
            made.state.setProperty (schema::deviceMix, mix, nullptr);

        if (const auto* devicePresets = saved[keys::devices].getArray())
        {
            for (const auto& device : *devicePresets)
            {
                const auto state = makeDevice (song, host, device);
                const auto fits = state.isValid() && ! isInstrumentState (state);
                made.devices.push_back (fits ? getDeviceID (state) : DeviceID());

                if (fits)
                    made.state.appendChild (state, nullptr);
            }
        }

        // Its eight macros, as a new container's (Containers.h: groupIntoContainer()), named and set as the preset's, with
        // what they modulate in it.
        const auto* macros = saved[keys::macros].getArray();

        for (int i = 0; i < macrosPerContainer; ++i)
        {
            const auto preset = macros != nullptr && i < macros->size() ? macros->getReference (i) : juce::var();
            const auto macroName = preset[keys::name].toString();
            juce::ValueTree macro (schema::MACRO);
            macro.setProperty (schema::modulatorID, (juce::int64) song.makeID(), nullptr);
            macro.setProperty (schema::macroName, macroName.isNotEmpty() ? macroName : "Macro " + juce::String (i + 1), nullptr);

            if (const auto value = juce::jlimit (0.0f, 1.0f, (float) preset[keys::value]); value > 0.0f)
                macro.setProperty (schema::macroValue, value, nullptr);

            if (const auto* targets = preset[keys::modulates].getArray())
            {
                for (const auto& target : *targets)
                {
                    const auto index = (int) target.getProperty (keys::device, -1);
                    const auto device =
                        juce::isPositiveAndBelow (index, (int) made.devices.size()) ? made.devices[(size_t) index] : DeviceID();
                    const ParameterRef parameter { device, target[keys::parameter].toString() };

                    if (device.isValid()
                        && hasParameter (made.state.getChildWithProperty (schema::deviceID, (juce::int64) device.getRaw()),
                                         parameter.parameter))
                        addModulates (macro, parameter, (float) target[keys::amount]);
                }
            }

            made.state.appendChild (macro, nullptr);
        }

        return made;
    }

    // What a modulator or a macro on the song modulates, from a preset; find() names a target's parameter.
    void applyModulations (Song& song, ModulatorID modulator, const juce::var& preset,
                           const std::function<ParameterRef (const juce::var&)>& find)
    {
        if (const auto* targets = preset[keys::modulates].getArray())
            for (const auto& target : *targets)
                if (const auto parameter = find (target); canModulate (song, modulator, parameter))
                    setModulation (song, modulator, parameter, juce::jlimit (-1.0f, 1.0f, (float) target[keys::amount]));
    }

    //==============================================================================
    // A preset's values put into one of Phazite's own devices of its type, each as a value set by hand, and a parameter it
    // doesn't name at its default, so that a preset is the same starting point whatever the device was set to; its sounds
    // too, copied into the project (or, unless copyFiles, left where they are), and the device's own sound where it names
    // none.
    void loadInto (Song& song, DeviceID id, const devices::DeviceInfo& info, const juce::var& saved, bool copyFiles = true)
    {
        const auto values = saved[keys::parameters];

        for (const auto& parameter : info.parameters)
        {
            const juce::Identifier parameterID (toString (parameter.id));
            const auto value = values.hasProperty (parameterID) ? (float) values[parameterID] : parameter.defaultValue;
            setValueSetByHand (song, ParameterRef { id, parameterID.toString() }, devices::constrain (parameter, value));
        }

        // Its sounds as a device of its type off the song holds them, so that their files are copied in.
        juce::ValueTree device (schema::DEVICE), sounds (schema::SOUNDS);
        device.setProperty (schema::deviceType, toString (info.typeID), nullptr);

        for (const auto& property : getSoundProperties (info))
            if (const auto sound = saved[keys::sounds][property].toString(); sound.isNotEmpty())
                sounds.setProperty (property, sound, nullptr);

        device.appendChild (sounds, nullptr);

        if (copyFiles)
        {
            copyDeviceFilesInto (song, device);
        }
        else
        {
            // Where they are, but those that aren't there any more, as copying leaves them out.
            for (const auto& property : getSoundProperties (info))
                if (const auto path = sounds[property].toString(); juce::File::isAbsolutePath (path) && ! juce::File (path).existsAsFile())
                    sounds.removeProperty (property, nullptr);
        }

        const auto copied = device.getChildWithName (schema::SOUNDS);

        for (const auto& property : getSoundProperties (info))
            if (copied.hasProperty (property))
                setDeviceSound (song, id, property, copied[property].toString());
            else if (! saved[keys::sounds].hasProperty (property))
                setDeviceSound (song, id, property, {}); // its own sound again
    }
} // namespace

//==============================================================================
juce::var makeDevicePreset (Song& song, DeviceID id)
{
    if (! findDeviceState (song, id).isValid())
        return {};

    // What its channel's plug-ins keep of their own, in their elements first.
    song.storeDeviceStates (getDeviceChannel (song, id).getRaw());
    const auto state = findDeviceState (song, id);
    auto preset = makeObject();

    if (isContainerState (state))
    {
        set (preset, keys::kind, keys::containerKind);
        set (preset, keys::container, describeContainer (song, state));
        return preset;
    }

    const auto described = describeDevice (song, state);

    if (! described.isObject())
        return {};

    set (preset, keys::kind, keys::deviceKind);
    set (preset, keys::device, described);
    return preset;
}

juce::var makeTrackPreset (Song& song, TrackID track)
{
    if (! findTrackState (song, track).hasType (schema::TRACK))
        return {};

    song.storeDeviceStates (track.getRaw());
    const auto state = findTrackState (song, track);
    auto saved = makeObject();
    set (saved, keys::name, getTrackName (song, track));
    set (saved, keys::colour, juce::Colour::fromString (state[schema::trackColour].toString()).toString());

    for (const auto& [property, key] :
         { std::pair { &schema::drumMode, &keys::drumMode }, std::pair { &schema::drumOrder, &keys::drumOrder },
           std::pair { &schema::drumHidden, &keys::drumHidden } })
        if (state.hasProperty (*property))
            set (saved, *key, state[*property]);

    set (saved, keys::volume, getChannelVolume (song, track));
    set (saved, keys::pan, getChannelPan (song, track));

    // Its instrument, and its effects and containers, in their order.
    const auto instrument = getInstrument (song, track);
    const auto effects = getEffects (song, track);

    if (instrument.isValid())
        set (saved, keys::instrument, describeDevice (song, findDeviceState (song, instrument)));

    juce::Array<juce::var> effectPresets;

    for (const auto effect : effects)
    {
        if (const auto effectState = findDeviceState (song, effect); isContainerState (effectState))
        {
            auto container = makeObject();
            set (container, keys::container, describeContainer (song, effectState));
            effectPresets.add (container);
        }
        else
        {
            effectPresets.add (describeDevice (song, effectState));
        }
    }

    set (saved, keys::effects, effectPresets);

    // Its modulators and macros, with what they modulate on it: its devices', those in its containers, its volume and pan.
    const auto fader = getFader (song, track);

    const auto onTrack = [&song, fader, instrument, &effects] (const juce::var& target, const ParameterRef& parameter)
    {
        if (parameter.device == fader)
        {
            set (target, keys::mixer, true);
            return true;
        }

        if (instrument.isValid() && parameter.device == instrument)
        {
            set (target, keys::instrument, true);
            return true;
        }

        for (size_t i = 0; i < effects.size(); ++i)
        {
            if (effects[i] == parameter.device)
            {
                set (target, keys::effect, (int) i);
                return true;
            }

            const auto inside = getContainerDevices (song, effects[i]);

            if (const auto found = std::find (inside.begin(), inside.end(), parameter.device); found != inside.end())
            {
                set (target, keys::effect, (int) i);
                set (target, keys::inside, (int) std::distance (inside.begin(), found));
                return true;
            }
        }

        return false; // a send, which the preset doesn't hold
    };

    juce::Array<juce::var> modulators, macros;

    for (const auto id : getModulators (song, track))
    {
        const auto modulator = findModulatorState (song, id);

        if (modulator.hasType (schema::MACRO))
        {
            macros.add (describeMacro (modulator, onTrack));
            continue;
        }

        const auto type = findModulatorType (modulator[schema::modulatorType].toString());
        const auto kind = getModulatorKind (modulator);

        if (! type.has_value() || ! kind.has_value())
            continue;

        auto preset = makeObject();
        set (preset, keys::type, keys::modulatorTypeNames[(size_t) *type]);

        // Its parameters' values set by hand (a preset loads it switched on).
        const auto values = modulator.getChildWithName (schema::VALUES);
        const auto parameters = makeObject();

        for (const auto& info : devices::getModulatorParameters (*kind))
        {
            const juce::Identifier parameterID (toString (info.id));
            set (parameters, parameterID, (float) values.getProperty (parameterID, info.defaultValue));
        }

        set (preset, keys::parameters, parameters);

        if (*kind == devices::ModulatorKind::steps)
        {
            auto steps = getModulatorSteps (modulator);
            steps.resize ((size_t) getNumSteps (modulator), 0.0f);
            juce::Array<juce::var> savedSteps;

            for (const auto step : steps)
                savedSteps.add (step);

            set (preset, keys::steps, savedSteps);
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

juce::ValueTree makeDeviceFromPreset (Song& song, const PluginHost& host, const juce::var& preset)
{
    const auto kind = findKind (preset[keys::kind].toString());

    if (kind == PresetKind::container)
        return makeContainer (song, host, preset[keys::container]).state;

    if (kind == PresetKind::device)
        return makeDevice (song, host, preset[keys::device]);

    return {};
}

DeviceID addDeviceFromPreset (Song& song, const PluginHost& host, TrackID channel, const juce::var& preset, Selection* selection)
{
    const auto device = makeDeviceFromPreset (song, host, preset);
    return device.isValid() ? placeDevice (song, channel, device, selection) : DeviceID();
}

TrackID addTrackFromPreset (Song& song, const PluginHost& host, TrackID after, const juce::var& preset, Selection* selection)
{
    const auto saved = preset[keys::track];

    if (! saved.isObject())
        return {};

    const auto track = insertTrack (song, after, selection);

    if (! track.isValid())
        return {};

    setTrackName (song, track, saved[keys::name].toString());

    if (const auto colour = saved[keys::colour].toString(); colour.isNotEmpty())
        setTrackColour (song, track, juce::Colour::fromString (colour));

    for (const auto& [property, key] :
         { std::pair { &schema::drumMode, &keys::drumMode }, std::pair { &schema::drumOrder, &keys::drumOrder },
           std::pair { &schema::drumHidden, &keys::drumHidden } })
        if (saved.hasProperty (*key))
            findTrackState (song, track).setProperty (*property, saved[*key], nullptr); // view state

    if (saved.hasProperty (keys::volume))
        setChannelVolume (song, track, (float) saved[keys::volume]);

    if (saved.hasProperty (keys::pan))
        setChannelPan (song, track, (float) saved[keys::pan]);

    // Its instrument, then its effects and containers in their order, each by its place in the preset (none for one that
    // couldn't be made, so that the others keep their places).
    DeviceID instrument;

    if (const auto device = saved[keys::instrument]; device.isObject())
        if (const auto state = makeDevice (song, host, device); state.isValid())
            instrument = setInstrument (song, track, state);

    std::vector<DeviceID> effects;
    std::vector<std::vector<DeviceID>> containersDevices;

    if (const auto* effectPresets = saved[keys::effects].getArray())
    {
        for (const auto& effect : *effectPresets)
        {
            DeviceID added;
            std::vector<DeviceID> inside;

            if (const auto container = effect[keys::container]; container.isObject())
            {
                auto made = makeContainer (song, host, container);

                if ((added = insertDeviceState (song, track, made.state)).isValid())
                    inside = std::move (made.devices);
            }
            else if (const auto state = makeDevice (song, host, effect); state.isValid() && ! isInstrumentState (state))
            {
                added = insertDeviceState (song, track, state);
            }

            effects.push_back (added);
            containersDevices.push_back (std::move (inside));
        }
    }

    // Its modulators and macros, then what they modulate.
    const auto find = [&song, track, instrument, &effects, &containersDevices] (const juce::var& target) -> ParameterRef
    {
        const auto id = target[keys::parameter].toString();

        if ((bool) target[keys::mixer])
        {
            if (id == "volume")
                return { getFader (song, track), getVolumeParameterID (song, track) };

            if (id == "pan")
                return { getFader (song, track), getPanParameterID (song, track) };

            return {};
        }

        if ((bool) target[keys::instrument])
            return { instrument, id };

        const auto effect = (int) target.getProperty (keys::effect, -1);

        if (! juce::isPositiveAndBelow (effect, (int) effects.size()))
            return {};

        if (target.hasProperty (keys::inside))
        {
            const auto& inside = containersDevices[(size_t) effect];
            const auto index = (int) target[keys::inside];
            return juce::isPositiveAndBelow (index, (int) inside.size()) ? ParameterRef { inside[(size_t) index], id } : ParameterRef();
        }

        return { effects[(size_t) effect], id };
    };

    if (const auto* modulators = saved[keys::modulators].getArray())
    {
        for (const auto& modulator : *modulators)
        {
            const auto type = findModulatorType (modulator[keys::type].toString());
            const auto added = type.has_value() ? addModulator (song, track, *type) : ModulatorID();
            const auto kind = getModulatorKind (findModulatorState (song, added));

            if (! kind.has_value())
                continue;

            if (auto* values = modulator[keys::parameters].getDynamicObject())
                for (const auto& [id, value] : values->getProperties())
                    if (devices::findModulatorParameter (*kind, id.toString().toStdString()) >= 0)
                        setValueSetByHand (song, ParameterRef { DeviceID::fromRaw (added.getRaw()), id.toString() }, (float) value);

            if (const auto* steps = modulator[keys::steps].getArray())
                for (int i = 0; i < steps->size(); ++i)
                    setModulatorStep (song, added, i, (float) steps->getReference (i));

            applyModulations (song, added, modulator, find);
        }
    }

    if (const auto* macros = saved[keys::macros].getArray())
    {
        for (const auto& macro : *macros)
        {
            const auto name = macro[keys::name].toString().trim();
            const auto added =
                name.isNotEmpty() ? addMacro (song, findTrackState (song, track), name) : addModulator (song, track, ModulatorType::macro);

            if (! added.isValid())
                continue;

            // A macro's value is its one parameter, by its ID (model/devices/internal/Parameters.h).
            setValueSetByHand (song, ParameterRef { DeviceID::fromRaw (added.getRaw()), juce::String (added.getRaw()) },
                               juce::jlimit (0.0f, 1.0f, (float) macro[keys::value]));
            applyModulations (song, added, macro, find);
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
    auto* song = findSong (context);
    const auto device = song != nullptr && id.isValid() ? findDeviceState (*song, id) : juce::ValueTree();

    if (! device.isValid())
        return {};

    return isContainerState (device) ? juce::String ("Containers") : juce::File::createLegalFileName (getDeviceName (*song, id));
}

bool saveDevicePreset (const ProjectContext& context, DeviceID id, const juce::File& file)
{
    auto* song = findSong (context);
    return song != nullptr && id.isValid() && findDeviceState (*song, id).isValid() && writePreset (makeDevicePreset (*song, id), file);
}

bool saveTrackPreset (const ProjectContext& context, TrackID id, const juce::File& file)
{
    // A track that holds clips: not a folder, a return or the master.
    auto* song = findSong (context);
    const auto state = song != nullptr && id.isValid() ? findTrackState (*song, id) : juce::ValueTree();
    return state.hasType (schema::TRACK) && ! isFolderState (state) && ! isReturnState (state)
           && writePreset (makeTrackPreset (*song, id), file);
}

bool loadPreset (ProjectContext& context, const juce::File& file, TrackID target)
{
    auto* song = findSong (context);
    const auto preset = song != nullptr ? readPreset (file) : juce::var();

    if (! preset.isObject() || (target.isValid() && ! findTrackState (*song, target).isValid()))
        return false;

    const auto name = file.getFileNameWithoutExtension();
    const UndoStep step (*song, "Add " + name);
    auto& selection = getSelection (context);
    const auto& host = getPluginHost (getEngineContext (context));

    if (findKind (preset[keys::kind].toString()) == PresetKind::track)
        return addTrackFromPreset (*song, host, target, preset, &selection).isValid();

    // A container is named after its preset.
    const auto device = addDeviceFromPreset (*song, host, target, preset, &selection);

    if (device.isValid() && isContainer (*song, device))
        setContainerName (*song, device, name);

    return device.isValid();
}

bool loadPresetInto (ProjectContext& context, DeviceID id, const juce::File& file)
{
    auto* song = findSong (context);
    auto device = song != nullptr && id.isValid() ? findDeviceState (*song, id) : juce::ValueTree();
    const auto preset = device.isValid() ? readPreset (file) : juce::var();

    if (! preset.isObject() || findKind (preset[keys::kind].toString()) != PresetKind::device)
        return false;

    const auto saved = preset[keys::device];
    const auto stepName = "Load Preset " + file.getFileNameWithoutExtension();

    if (const auto* own = findOwnInfo (device))
    {
        if (saved[keys::type].toString() != toString (own->typeID))
            return false;

        const UndoStep step (*song, stepName);
        loadInto (*song, id, *own, saved);
        return true;
    }

    if (! isPluginState (device) || ! saved[keys::plugin].isObject() || ! isSamePlugin (device, saved[keys::plugin]))
        return false;

    // A plug-in keeps its settings in a block of its own, which its element holds once stored: stored first, outside the
    // undo history, so that an undo puts back the settings it has now. Its instance takes the preset's program and settings
    // from the song, and the ones before them again on undo (model/plugins/internal/PluginInstances.h).
    if (auto* session = getSession (context))
        session->getPlugins().store (id);

    const UndoStep step (*song, stepName);
    auto* undo = &song->getUndoManager();

    if (saved.hasProperty (keys::program))
        device.setProperty (schema::pluginProgram, (int) saved[keys::program], undo);

    if (const auto settings = saved[keys::state].toString(); settings.isNotEmpty())
        device.setProperty (schema::pluginState, settings, undo);
    else
        device.removeProperty (schema::pluginState, undo);

    if (saved.hasProperty (keys::mix))
        setMix (*song, id, (float) saved[keys::mix]);

    return true;
}

bool isPresetFor (const Song& song, DeviceID id, const juce::File& file)
{
    const auto* own = findOwnInfo (findDeviceState (song, id));

    if (own == nullptr || ! isPresetFile (file))
        return false;

    const auto preset = readPreset (file);
    return findKind (preset[keys::kind].toString()) == PresetKind::device
           && preset[keys::device][keys::type].toString() == toString (own->typeID);
}

bool tryPresetIn (Song& song, DeviceID id, const juce::File& file)
{
    if (! isPresetFor (song, id, file))
        return false;

    loadInto (song, id, *findOwnInfo (findDeviceState (song, id)), readPreset (file)[keys::device], false);
    return true;
}
} // namespace phazite
