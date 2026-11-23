// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Parameters.h"
#include "engine/nodes/Channel.h"
#include "engine/nodes/Container.h"
#include "internal/Devices.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/ModulationValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SendsValue.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginInstances.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace phazite
{
namespace
{
    juce::String toString (std::string_view text)
    {
        return juce::String (text.data(), text.size());
    }

    std::uint64_t readID (const juce::ValueTree& state, const juce::Identifier& property)
    {
        return (std::uint64_t) (juce::int64) state.getProperty (property, (juce::int64) 0);
    }

    // A plug-in's parameter in its instance (none while no session plays the song, or the plug-in doesn't load): its
    // instance and its place there.
    std::pair<SandboxedInstance*, int> findPluginParameter (const Song& song, const ParameterRef& ref)
    {
        auto* instance = findPluginInstance (song, ref.device);
        const auto index = instance != nullptr ? instance->findParameter (ref.parameter) : -1;
        return { index >= 0 ? instance : nullptr, index };
    }

    // The VALUE of a plug-in's element that holds a parameter's value set by hand, if it has one.
    juce::ValueTree findPluginValue (const juce::ValueTree& device, const juce::String& parameter)
    {
        return device.getChildWithProperty (schema::valueParameter, parameter);
    }

    // Whether two of a plug-in's values, from 0 to 1, are the same, to a millionth (as isDefaultValue() takes them).
    bool isSamePluginValue (float a, float b)
    {
        return std::abs (a - b) <= 1.0e-6f;
    }

    // Whether a value is a plug-in's parameter's default, as its instance tells it (none known without one).
    bool isPluginDefault (const Song& song, const ParameterRef& ref, float value)
    {
        const auto [instance, index] = findPluginParameter (song, ref);
        return instance != nullptr && isSamePluginValue (value, instance->getParameterInfos()[(size_t) index].defaultValue);
    }

    bool isVCA (const juce::ValueTree& channel, DeviceID fader)
    {
        return readID (channel, schema::channelVCA) == fader.getRaw();
    }

    // A modulator or a macro of a parameter found (a modulator's parameter, a macro's value), by its ID: a look in modulation's
    // area (model/modulation/internal/Modulation.h), as its FoundParameter holds no element of it.
    std::optional<ModulatorValue> findModulatorOf (const Song& song, const ParameterRef& ref)
    {
        return getModulatorValue (song, ModulatorID::fromRaw (ref.device.getRaw()));
    }

    // Whether a parameter's description is one of a modulator kind's (a FoundParameter's info: the kind it was found of).
    bool isParameterOf (devices::ModulatorKind kind, const devices::ParameterInfo* info)
    {
        const auto parameters = devices::getModulatorParameters (kind);
        return std::any_of (parameters.begin(), parameters.end(), [info] (const devices::ParameterInfo& each) { return &each == info; });
    }

    std::optional<FoundParameter> found (const ParameterRef& ref, ParameterSource kind, const juce::ValueTree& state,
                                         const devices::ParameterInfo* info = nullptr)
    {
        return FoundParameter { ref, kind, state, info };
    }

    // A chain's place among its container's, and how many chains it has; and whether it has a crossover: a band's, but the
    // first's (which starts at the bottom), in a container split into bands.
    std::pair<int, int> getChainPlace (const juce::ValueTree& chain)
    {
        const auto chains = getChainStates (chain.getParent());
        return { (int) (std::find (chains.begin(), chains.end(), chain) - chains.begin()), (int) chains.size() };
    }

    bool hasCrossover (const juce::ValueTree& chain)
    {
        return readContainerSplit (chain.getParent()) == ContainerSplit::bands && getChainPlace (chain).first > 0;
    }

    // A chain's crossover, its own or its place's default; and the ones it stays between: the chain's before it (the bottom,
    // for the second chain) and the one's after it (the top, for the last).
    float getCrossover (const juce::ValueTree& chain)
    {
        const auto [index, count] = getChainPlace (chain);
        return readChainCrossover (chain).value_or (getDefaultCrossover (index, count));
    }

    juce::Range<float> getCrossoverRoom (const juce::ValueTree& chain)
    {
        const auto chains = getChainStates (chain.getParent());
        const auto index = getChainPlace (chain).first;
        const auto below = index > 1 ? getCrossover (chains[(size_t) index - 1]) : lowestCrossover;
        const auto above = index + 1 < (int) chains.size() ? getCrossover (chains[(size_t) index + 1]) : highestCrossover;
        return { below, std::max (below, above) };
    }

    juce::String describeFrequency (float hertz)
    {
        return hertz < 1000.0f ? juce::String (juce::roundToInt (hertz)) + " Hz"
                               : juce::String (hertz / 1000.0f, hertz < 10000.0f ? 2 : 1) + " kHz";
    }
} // namespace

//==============================================================================
std::optional<FoundParameter> locateParameter (const Song& song, const ParameterRef& ref)
{
    if (! ref.isValid())
        return {};

    // A modulator's or a macro's first: a look in modulation's area by its ID (its FoundParameter holds no element: the song's
    // tree holds none of it), which allocates nothing, before the tree's walks (locateParameterInTree()), which a macro's turn
    // (the automation's watcher asks at each step) would otherwise pay for every track. A reference to a device, a chain or a
    // fader looks there for an ID no modulator has (the song's IDs are each one item's), and finds none whatever the area holds.
    if (const auto modulator = findModulatorOf (song, ref))
    {
        // A macro's one parameter has the macro's ID.
        if (modulator->isMacro())
            return ref.parameter == juce::String (ref.device.getRaw()) ? found (ref, ParameterSource::macro, {}) : std::nullopt;

        if (const auto index = devices::findModulatorParameter (*modulator->kind, ref.parameter.toStdString()); index >= 0)
            return found (ref, ParameterSource::modulator, {}, &devices::getModulatorParameters (*modulator->kind)[(size_t) index]);

        return {};
    }

    return locateParameterInTree (song, ref);
}

std::optional<FoundParameter> locateParameterInTree (const Song& song, const ParameterRef& ref)
{
    if (! ref.isValid())
        return {};

    // A fader's first, which controls read most.
    if (const auto channel = findChannelOfFader (song, ref.device); channel.isValid())
    {
        const auto isMaster = channel.hasType (schema::MASTER);

        if (isVCA (channel, ref.device))
            return ref.parameter == schema::vcaVolume ? found (ref, ParameterSource::volume, channel) : std::nullopt;

        if (ref.parameter == (isMaster ? schema::masterVolume : schema::faderVolume))
            return found (ref, ParameterSource::volume, channel);

        if (ref.parameter == (isMaster ? schema::masterPan : schema::faderPan))
            return found (ref, ParameterSource::pan, channel);

        return {};
    }

    if (const auto device = findDeviceState (song, ref.device); device.isValid())
    {
        if (isPluginState (device))
        {
            // One its instance has, or one the song keeps a value of (a plug-in that doesn't load keeps what refers to it).
            const auto there = findPluginParameter (song, ref).first != nullptr || findPluginValue (device, ref.parameter).isValid();
            return there ? found (ref, ParameterSource::plugin, device) : std::nullopt;
        }

        // One of Phazite's own (or of a type a test registered: model/devices/internal/Devices.h).
        const auto* info = findOwnDevice (readDeviceType (device).toStdString());

        if (info != nullptr)
            if (const auto index = devices::findParameter (*info, ref.parameter.toStdString()); index >= 0)
                return found (ref, ParameterSource::device, device, &info->parameters[(size_t) index]);

        return {};
    }

    // A send's level, by the send's value: its element the send's track's, as a fader's is its channel's. Its level is a send's
    // one parameter (where it goes and whether it is before the fader are no parameters), and no chain or modulator has a
    // send's ID, so only a reference to that parameter asks the sends: one that names a chain's or a modulator's, or a
    // device gone, reads no send (a tree callback that looks for it again, MidiLinker's, reaches no sends' reader).
    if (ref.parameter == schema::sendLevelParameter)
        if (const auto send = getSendValue (song, ref.device))
            return found (ref, ParameterSource::send, findTrackState (song, send->from));

    // A container's chain's volume and pan, and a band's crossover.
    if (const auto chain = findContainerChain (song, ref.device); chain.isValid())
    {
        if (ref.parameter == schema::chainVolumeParameter)
            return found (ref, ParameterSource::chainVolume, chain);

        if (ref.parameter == schema::chainPanParameter)
            return found (ref, ParameterSource::chainPan, chain);

        if (ref.parameter == schema::crossoverParameter && hasCrossover (chain))
            return found (ref, ParameterSource::crossover, chain, &engine::crossoverInfo);

        return {};
    }

    return {};
}

bool isStillThere (const Song& song, const FoundParameter& parameter)
{
    // A modulator's or a macro's by its ID, still of the kind it was found of (a merge's choice can give it another).
    if (parameter.source == ParameterSource::modulator || parameter.source == ParameterSource::macro)
    {
        const auto modulator = findModulatorOf (song, parameter.ref);

        if (! modulator.has_value())
            return false;

        return parameter.source == ParameterSource::macro ? modulator->isMacro()
                                                          : modulator->kind.has_value() && isParameterOf (*modulator->kind, parameter.info);
    }

    if (! parameter.state.isAChildOf (song.getState()))
        return false;

    // Its element still names it: a folder's fader of the other kind, for one, has another ID.
    const auto id = parameter.ref.device.getRaw();

    switch (parameter.source)
    {
        case ParameterSource::device:
        case ParameterSource::plugin:
            return readDeviceElementID (parameter.state).getRaw() == id;
        case ParameterSource::volume:
        case ParameterSource::pan:
            return readID (parameter.state, schema::channelFader) == id || readID (parameter.state, schema::channelVCA) == id;
        case ParameterSource::send:
            return findTrackSend (parameter.state, parameter.ref.device).has_value(); // on the track it was found on, one look
        case ParameterSource::modulator:
        case ParameterSource::macro:
            break; // above
        case ParameterSource::chainVolume:
        case ParameterSource::chainPan:
            return readDeviceElementID (parameter.state).getRaw() == id;
        case ParameterSource::crossover:
            return readDeviceElementID (parameter.state).getRaw() == id && hasCrossover (parameter.state);
    }

    return false;
}

//==============================================================================
ParameterDescription describeParameter (const Song& song, const FoundParameter& parameter)
{
    const auto& state = parameter.state;
    const auto atZeroDecibels = engine::decibelsToFaderPosition (0.0f);

    switch (parameter.source)
    {
        case ParameterSource::device:
        case ParameterSource::modulator:
        {
            const auto& info = *parameter.info;
            return { parameter.source, toString (info.name), devices::getRange (info), info.defaultValue, info };
        }

        case ParameterSource::volume:
        {
            // The master's starts a little down (defaultMasterVolume), where a double-click sets it back.
            const auto isMaster = state.hasType (schema::MASTER);
            const auto* name = isMaster ? "Master volume" : isVCA (state, parameter.ref.device) ? "VCA" : "Volume";
            return { parameter.source,
                     name,
                     { 0.0f, 1.0f },
                     isMaster ? engine::decibelsToFaderPosition (defaultMasterVolume) : atZeroDecibels,
                     {} };
        }

        case ParameterSource::pan:
            return { parameter.source, state.hasType (schema::MASTER) ? "Master pan" : "Pan", { -1.0f, 1.0f }, 0.0f, {} };

        case ParameterSource::send:
            return { parameter.source, "Send level", { 0.0f, 1.0f }, atZeroDecibels, {} };

        case ParameterSource::macro:
        {
            const auto macro = findModulatorOf (song, parameter.ref);
            return { parameter.source, macro.has_value() ? macro->name : juce::String(), { 0.0f, 1.0f }, 0.0f, {} };
        }

        case ParameterSource::chainVolume:
            return { parameter.source, "Volume", { 0.0f, 1.0f }, atZeroDecibels, {} };

        case ParameterSource::chainPan:
            return { parameter.source, "Pan", { -1.0f, 1.0f }, 0.0f, {} };

        case ParameterSource::crossover:
        {
            const auto [index, count] = getChainPlace (state);
            return { parameter.source, "Crossover", devices::getRange (engine::crossoverInfo), getDefaultCrossover (index, count),
                     engine::crossoverInfo };
        }

        case ParameterSource::plugin:
        {
            // From 0 to 1, in its steps if it says it has some, as the plug-in told it as it opened.
            if (const auto [instance, index] = findPluginParameter (song, parameter.ref); instance != nullptr)
            {
                const auto& info = instance->getParameterInfos()[(size_t) index];
                const auto step = info.numSteps > 1 ? 1.0f / (float) (info.numSteps - 1) : 0.0f;
                return { parameter.source, info.name, { 0.0f, 1.0f, step }, info.defaultValue, {} };
            }

            return { parameter.source, parameter.ref.parameter, { 0.0f, 1.0f }, {}, {} };
        }
    }

    return { parameter.source, {}, {}, {}, {} };
}

juce::String getParameterText (const Song& song, const FoundParameter& parameter, float value, std::function<void()> whenKnown)
{
    switch (parameter.source)
    {
        case ParameterSource::volume:
        case ParameterSource::chainVolume:
            return juce::Decibels::toString (engine::faderPositionToDecibels (value) + 0.001f);

        case ParameterSource::send:
            return juce::Decibels::toString (engine::faderPositionToDecibels (value));

        case ParameterSource::crossover:
            return describeFrequency (value);

        case ParameterSource::pan:
        case ParameterSource::chainPan:
            if (std::abs (value) < 0.001f)
                return "Centre";

            return value < 0.0f ? juce::String (value, 3) + " Left" : "+" + juce::String (value, 3) + " Right";

        case ParameterSource::device:
        case ParameterSource::modulator:
            // A choice reads as its name ("Sine"), and a count (one that moves in whole steps) as a whole number.
            if (const auto& info = *parameter.info; info.unit == devices::Unit::choice && ! info.choices.empty())
                return toString (
                    info.choices[(size_t) juce::jlimit (0, (int) info.choices.size() - 1, juce::roundToInt (value - info.minimum))]);

            if (const auto step = parameter.info->step; step.has_value() && juce::exactlyEqual (*step, std::round (*step)))
                return juce::String (juce::roundToInt (value));

            return juce::String (value, 3);

        case ParameterSource::macro:
            return juce::String (value, 3);

        case ParameterSource::plugin:
            if (const auto [instance, index] = findPluginParameter (song, parameter.ref); instance != nullptr)
                return instance->describeValue (index, value, std::move (whenKnown));

            return juce::String (value, 2);
    }

    return {};
}

float getValueSetByHand (const Song& song, const FoundParameter& parameter)
{
    const auto& state = parameter.state;

    switch (parameter.source)
    {
        case ParameterSource::device:
            // By the name the reference holds, which JUCE's pool of names has: no text made at each tick.
            return readDeviceParameterValue (state, *parameter.info, juce::Identifier (parameter.ref.parameter));

        case ParameterSource::modulator:
        {
            // Its value's, by the parameter's place among its kind's; the parameter's default for one it hasn't.
            const auto modulator = findModulatorOf (song, parameter.ref);

            if (! modulator.has_value() || modulator->isMacro())
                return parameter.info->defaultValue;

            const auto index = devices::findModulatorParameter (*modulator->kind, parameter.ref.parameter.toStdString());
            return index >= 0 ? modulator->getParameterValue (index) : parameter.info->defaultValue;
        }

        case ParameterSource::volume:
            return engine::decibelsToFaderPosition (
                (float) state.getProperty (schema::channelVolume, state.hasType (schema::MASTER) ? defaultMasterVolume : 0.0f));

        case ParameterSource::pan:
            return (float) state.getProperty (schema::channelPan, 0.0f);

        case ParameterSource::send:
        {
            const auto send = findTrackSend (state, parameter.ref.device); // on the track it was found on, one look
            return engine::decibelsToFaderPosition (send.has_value() ? send->level : 0.0f);
        }

        case ParameterSource::macro:
        {
            const auto macro = findModulatorOf (song, parameter.ref);
            return macro.has_value() ? macro->value : 0.0f;
        }

        case ParameterSource::chainVolume:
            return engine::decibelsToFaderPosition (readChainVolume (state));

        case ParameterSource::chainPan:
            return readChainPan (state);

        case ParameterSource::crossover:
            return getCrossover (state);

        case ParameterSource::plugin:
            // The song's, where it sets one; else its instance's.
            if (const auto stored = findPluginValue (state, parameter.ref.parameter); stored.isValid())
                return (float) stored[schema::valueSetByHand];

            if (const auto [instance, index] = findPluginParameter (song, parameter.ref); instance != nullptr)
                return instance->getParameterValue (index);

            break;
    }

    return 0.0f;
}

void setValueSetByHand (Song& song, const FoundParameter& parameter, float value)
{
    // A value typed, or a plug-in's, that is no finite number changes nothing: juce::jlimit() would pass a NaN on (known
    // bug 148).
    if (! std::isfinite (value))
        return;

    const auto& ref = parameter.ref;
    auto* undo = &song.getUndoManager();
    auto state = parameter.state;
    auto told = value;

    switch (parameter.source)
    {
        case ParameterSource::device:
        {
            told = juce::jlimit (parameter.info->minimum, parameter.info->maximum, value);
            moveOnlyPoint (song, ref, told);
            keepValueSetByHand (state, *parameter.info, told, undo);
            break;
        }

        case ParameterSource::modulator:
        {
            // Nothing moves a modulator's parameters: no curve of one point follows them. Its value, through modulation's code.
            told = devices::constrain (*parameter.info, value);
            setModulatorValueSetByHand (song, ModulatorID::fromRaw (ref.device.getRaw()), *parameter.info, told);
            break;
        }

        case ParameterSource::plugin:
        {
            // A VALUE of the plug-in's element, which its instance takes (model/plugins/internal/PluginInstances.h), made
            // holding the value the plug-in has, so that an undo takes it back there; none for the value it has already.
            told = juce::jlimit (0.0f, 1.0f, value);
            moveOnlyPoint (song, ref, told);
            auto stored = findPluginValue (state, ref.parameter);

            if (! stored.isValid())
            {
                const auto held = getValueSetByHand (song, parameter);

                if (! isSamePluginValue (held, told))
                {
                    stored =
                        juce::ValueTree (schema::VALUE, { { schema::valueParameter, ref.parameter }, { schema::valueSetByHand, held } });
                    state.appendChild (stored, undo);
                    stored.setProperty (schema::valueSetByHand, told, undo);
                }

                break;
            }

            stored.setProperty (schema::valueSetByHand, told, undo);

            // One set back to the parameter's default goes, as Phazite's own devices keep none at their defaults
            // (keepValueSetByHand()): the plug-in holds the value then, which its settings hold once the next save stores
            // them. A change outside the song's history, so that the project saves it; the song's, not the plug-in's own,
            // which the versions would take its stored settings for a change of (model/base/Song.h).
            if (isPluginDefault (song, ref, told))
            {
                state.removeChild (stored, undo);
                song.markChangedOutsideHistory();
            }

            break;
        }

        case ParameterSource::volume:
        {
            told = juce::jlimit (0.0f, 1.0f, value);
            moveOnlyPoint (song, ref, told);
            setChannelVolume (song, getTrackID (state), engine::faderPositionToDecibels (told));
            break;
        }

        case ParameterSource::pan:
        {
            told = juce::jlimit (-1.0f, 1.0f, value);
            moveOnlyPoint (song, ref, told);
            setChannelPan (song, getTrackID (state), told);
            break;
        }

        case ParameterSource::send:
        {
            told = juce::jlimit (0.0f, 1.0f, value);
            moveOnlyPoint (song, ref, told);
            setSendLevel (song, ref.device, engine::faderPositionToDecibels (told));
            break;
        }

        case ParameterSource::macro:
        {
            told = juce::jlimit (0.0f, 1.0f, value);
            setMacroValue (song, ModulatorID::fromRaw (ref.device.getRaw()), told);
            break;
        }

        case ParameterSource::chainVolume:
        {
            told = juce::jlimit (0.0f, 1.0f, value);
            moveOnlyPoint (song, ref, told);
            setChainVolume (song, ref.device, engine::faderPositionToDecibels (told));
            break;
        }

        case ParameterSource::chainPan:
        {
            told = juce::jlimit (-1.0f, 1.0f, value);
            moveOnlyPoint (song, ref, told);
            setChainPan (song, ref.device, told);
            break;
        }

        case ParameterSource::crossover:
        {
            // Between its neighbours' crossovers: a band can't pass the one beside it.
            told = getCrossoverRoom (state).clipValue (juce::jlimit (lowestCrossover, highestCrossover, value));
            moveOnlyPoint (song, ref, told);
            setChainCrossover (song, ref.device, told);
            break;
        }
    }

    song.tellValueSetByHand (ref, told, false);
}

float getCurrentValue (const Song& song, const FoundParameter& parameter, bool withModulation)
{
    // Nothing moves a modulator's parameters or a macro's value.
    if (parameter.source == ParameterSource::modulator || parameter.source == ParameterSource::macro)
        return getValueSetByHand (song, parameter);

    // While Phazite's engine plays the song: what it played, or the value set by hand for one that nothing moves.
    if (const auto* session = findPlayingSession (song))
    {
        if (! session->getPlayer().isMoved (parameter.ref))
            return getValueSetByHand (song, parameter);

        if (const auto played = session->getPlayer().getPlayedValue (parameter.ref, withModulation))
            return *played;
    }

    return getValueSetByHand (song, parameter);
}

//==============================================================================
std::optional<ParameterDescription> describeParameter (const Song& song, const ParameterRef& ref)
{
    const auto parameter = locateParameter (song, ref);
    return parameter.has_value() ? std::optional (describeParameter (song, *parameter)) : std::nullopt;
}

juce::String getParameterText (const Song& song, const ParameterRef& ref, float value)
{
    const auto parameter = locateParameter (song, ref);
    return parameter.has_value() ? getParameterText (song, *parameter, value) : juce::String();
}

std::optional<devices::Travel> getControlTravel (const Song& song, const ParameterRef& ref)
{
    if (! ref.isValid())
        return {};

    // A plug-in's from 0 to 1, in the steps it says it has once it has loaded (describeParameter()).
    if (isPlugin (song, ref.device))
    {
        const auto description = describeParameter (song, ref);
        return devices::Travel { .step = description.has_value() ? description->range.interval : 0.0f };
    }

    if (ref.parameter == schema::crossoverParameter && findContainerChain (song, ref.device).isValid())
        return devices::getTravel (engine::crossoverInfo);

    const auto found = locateParameter (song, ref);

    if (! found.has_value())
        return {};

    if (found->info != nullptr)
        return devices::getTravel (*found->info);

    // The mixer's controls and a macro: linear, from their ranges' starts to their ends.
    const auto range = describeParameter (song, *found).range;
    return devices::Travel { range.start, range.end };
}

float getValueSetByHand (const Song& song, const ParameterRef& ref)
{
    const auto parameter = locateParameter (song, ref);
    return parameter.has_value() ? getValueSetByHand (song, *parameter) : 0.0f;
}

void setValueSetByHand (Song& song, const ParameterRef& ref, float value)
{
    if (const auto parameter = locateParameter (song, ref))
        setValueSetByHand (song, *parameter, value);
}

juce::String getChangeName (const Song& song, const ParameterRef& ref)
{
    const auto parameter = locateParameter (song, ref);

    if (! parameter.has_value())
        return {};

    switch (parameter->source)
    {
        case ParameterSource::volume:
            return parameter->state.hasType (schema::MASTER) ? "Change Master Volume" : "Change Volume";

        case ParameterSource::pan:
            return "Change Pan";

        case ParameterSource::send:
            return "Change Send Level";

        case ParameterSource::macro:
            return "Change " + getModulatorName (song, ModulatorID::fromRaw (ref.device.getRaw()));

        case ParameterSource::chainVolume:
        case ParameterSource::chainPan:
        case ParameterSource::crossover:
        case ParameterSource::device:
        case ParameterSource::plugin:
        case ParameterSource::modulator:
            break;
    }

    return "Change " + getParameterName (song, ref);
}

ManyValuesSetByHand::ManyValuesSetByHand (const Song& song)
{
    if (auto* session = findSession (song))
    {
        writer = &session->getWriter();
        writer->beginManyValues();
    }
}

ManyValuesSetByHand::~ManyValuesSetByHand()
{
    if (writer != nullptr)
        writer->endManyValues();
}

float getCurrentValue (const Song& song, const ParameterRef& ref, bool withModulation)
{
    const auto parameter = locateParameter (song, ref);
    return parameter.has_value() ? getCurrentValue (song, *parameter, withModulation) : 0.0f;
}

//==============================================================================
void keepValueSetByHand (juce::ValueTree element, const devices::ParameterInfo& info, float value, juce::UndoManager* undo)
{
    const juce::Identifier parameter (toString (info.id));

    if (! isDefaultValue (info, value))
    {
        element.getOrCreateChildWithName (schema::VALUES, undo).setProperty (parameter, toSavedValue (info, value), undo);
        return;
    }

    if (auto values = element.getChildWithName (schema::VALUES); values.isValid())
    {
        values.removeProperty (parameter, undo);

        if (values.getNumProperties() == 0 && values.getNumChildren() == 0)
            element.removeChild (values, undo);
    }
}

float readValueSetByHand (const juce::ValueTree& element, const devices::ParameterInfo& info)
{
    return readSavedValue (info, element.getChildWithName (schema::VALUES).getProperty (juce::Identifier (toString (info.id))));
}
} // namespace phazite
