// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Parameters.h"
#include "engine/nodes/Channel.h"
#include "engine/nodes/Container.h"
#include "internal/Devices.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginInstances.h"

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

    bool isVCA (const juce::ValueTree& channel, DeviceID fader)
    {
        return readID (channel, schema::channelVCA) == fader.getRaw();
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
        return chain.getParent()[schema::containerSplit].toString() == schema::splitBands && getChainPlace (chain).first > 0;
    }

    // A chain's crossover, its own or its place's default; and the ones it stays between: the chain's before it (the bottom,
    // for the second chain) and the one's after it (the top, for the last).
    float getCrossover (const juce::ValueTree& chain)
    {
        const auto [index, count] = getChainPlace (chain);
        return (float) chain.getProperty (schema::chainCrossover, getDefaultCrossover (index, count));
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

    // A parameter whose curve has one point plays that point's value everywhere: the point moves to a value set by hand,
    // in the same step, as a control's change.
    void moveOnePoint (Song& song, const ParameterRef& ref, float value)
    {
        // Not while automation is written by hand (model/playback/internal/AutomationWriter.h), which writes its own points.
        if (auto* session = findSession (song); session != nullptr && session->getWriter().isWriting (ref))
            return;

        if (auto curve = findCurveState (song, ref); curve.getNumChildren() == 1)
            if (auto point = curve.getChild (0); ! juce::exactlyEqual ((float) point[schema::pointValue], value))
                point.setProperty (schema::pointValue, value, &song.getUndoManager());
    }
} // namespace

//==============================================================================
std::optional<FoundParameter> locateParameter (const Song& song, const ParameterRef& ref)
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
        const auto* info = findOwnDevice (device[schema::deviceType].toString().toStdString());

        if (info != nullptr)
            if (const auto index = devices::findParameter (*info, ref.parameter.toStdString()); index >= 0)
                return found (ref, ParameterSource::device, device, &info->parameters[(size_t) index]);

        return {};
    }

    if (const auto send = findSendState (song, ref.device); send.isValid())
        return ref.parameter == schema::sendLevelParameter ? found (ref, ParameterSource::send, send) : std::nullopt;

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

    if (const auto modulator = findModulatorState (song, ModulatorID::fromRaw (ref.device.getRaw())); modulator.isValid())
    {
        // A macro's one parameter has the macro's ID.
        if (modulator.hasType (schema::MACRO))
            return ref.parameter == juce::String (ref.device.getRaw()) ? found (ref, ParameterSource::macro, modulator) : std::nullopt;

        if (const auto kind = getModulatorKind (modulator))
            if (const auto index = devices::findModulatorParameter (*kind, ref.parameter.toStdString()); index >= 0)
                return found (ref, ParameterSource::modulator, modulator, &devices::getModulatorParameters (*kind)[(size_t) index]);

        return {};
    }

    return {};
}

bool isStillThere (const Song& song, const FoundParameter& parameter)
{
    if (! parameter.state.isAChildOf (song.getState()))
        return false;

    // Its element still names it: a folder's fader of the other kind, for one, has another ID.
    const auto id = parameter.ref.device.getRaw();

    switch (parameter.source)
    {
        case ParameterSource::device:
        case ParameterSource::plugin:
            return readID (parameter.state, schema::deviceID) == id;
        case ParameterSource::volume:
        case ParameterSource::pan:
            return readID (parameter.state, schema::channelFader) == id || readID (parameter.state, schema::channelVCA) == id;
        case ParameterSource::send:
            return readID (parameter.state, schema::sendID) == id;
        case ParameterSource::modulator:
        case ParameterSource::macro:
            return readID (parameter.state, schema::modulatorID) == id;
        case ParameterSource::chainVolume:
        case ParameterSource::chainPan:
            return readID (parameter.state, schema::chainID) == id;
        case ParameterSource::crossover:
            return readID (parameter.state, schema::chainID) == id && hasCrossover (parameter.state);
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
            const auto* name = state.hasType (schema::MASTER) ? "Master volume" : isVCA (state, parameter.ref.device) ? "VCA" : "Volume";
            return { parameter.source, name, { 0.0f, 1.0f }, atZeroDecibels, {} };
        }

        case ParameterSource::pan:
            return { parameter.source, state.hasType (schema::MASTER) ? "Master pan" : "Pan", { -1.0f, 1.0f }, 0.0f, {} };

        case ParameterSource::send:
            return { parameter.source, "Send level", { 0.0f, 1.0f }, atZeroDecibels, {} };

        case ParameterSource::macro:
            return { parameter.source, state[schema::macroName].toString(), { 0.0f, 1.0f }, 0.0f, {} };

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

juce::String getParameterText (const Song& song, const FoundParameter& parameter, float value)
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
                return instance->describeValue (index, value);

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
        case ParameterSource::modulator:
            return (float) state.getChildWithName (schema::VALUES)
                .getProperty (juce::Identifier (parameter.ref.parameter), parameter.info->defaultValue);

        case ParameterSource::volume:
            return engine::decibelsToFaderPosition (
                (float) state.getProperty (schema::channelVolume, state.hasType (schema::MASTER) ? defaultMasterVolume : 0.0f));

        case ParameterSource::pan:
            return (float) state.getProperty (schema::channelPan, 0.0f);

        case ParameterSource::send:
            return engine::decibelsToFaderPosition ((float) state.getProperty (schema::sendLevel, 0.0f));

        case ParameterSource::macro:
            return (float) state.getProperty (schema::macroValue, 0.0f);

        case ParameterSource::chainVolume:
            return engine::decibelsToFaderPosition ((float) state.getProperty (schema::channelVolume, 0.0f));

        case ParameterSource::chainPan:
            return (float) state.getProperty (schema::channelPan, 0.0f);

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
    const auto& ref = parameter.ref;
    auto* undo = &song.getUndoManager();
    auto state = parameter.state;
    auto told = value;

    switch (parameter.source)
    {
        case ParameterSource::device:
        {
            told = juce::jlimit (parameter.info->minimum, parameter.info->maximum, value);
            moveOnePoint (song, ref, told);
            keepValueSetByHand (state, *parameter.info, told, undo);
            break;
        }

        case ParameterSource::modulator:
        {
            // Nothing moves a modulator's parameters: no curve of one point follows them.
            told = devices::constrain (*parameter.info, value);
            keepValueSetByHand (state, *parameter.info, told, undo);
            break;
        }

        case ParameterSource::plugin:
        {
            // A VALUE of the plug-in's element, which its instance takes (model/plugins/internal/PluginInstances.h).
            told = juce::jlimit (0.0f, 1.0f, value);
            moveOnePoint (song, ref, told);
            auto stored = state.getChildWithProperty (schema::valueParameter, ref.parameter);

            if (! stored.isValid())
            {
                stored = juce::ValueTree (schema::VALUE, { { schema::valueParameter, ref.parameter } });
                state.appendChild (stored, undo);
            }

            stored.setProperty (schema::valueSetByHand, told, undo);
            break;
        }

        case ParameterSource::volume:
        {
            told = juce::jlimit (0.0f, 1.0f, value);
            moveOnePoint (song, ref, told);
            setChannelVolume (song, getTrackID (state), engine::faderPositionToDecibels (told));
            break;
        }

        case ParameterSource::pan:
        {
            told = juce::jlimit (-1.0f, 1.0f, value);
            moveOnePoint (song, ref, told);
            setChannelPan (song, getTrackID (state), told);
            break;
        }

        case ParameterSource::send:
        {
            told = juce::jlimit (0.0f, 1.0f, value);
            moveOnePoint (song, ref, told);
            setSendLevel (song, ref.device, engine::faderPositionToDecibels (told));
            break;
        }

        case ParameterSource::macro:
        {
            if (told = juce::jlimit (0.0f, 1.0f, value); juce::exactlyEqual (told, 0.0f))
                state.removeProperty (schema::macroValue, undo);
            else
                state.setProperty (schema::macroValue, told, undo);

            break;
        }

        case ParameterSource::chainVolume:
        {
            told = juce::jlimit (0.0f, 1.0f, value);
            moveOnePoint (song, ref, told);
            setChainVolume (song, ref.device, engine::faderPositionToDecibels (told));
            break;
        }

        case ParameterSource::chainPan:
        {
            told = juce::jlimit (-1.0f, 1.0f, value);
            moveOnePoint (song, ref, told);
            setChainPan (song, ref.device, told);
            break;
        }

        case ParameterSource::crossover:
        {
            // Between its neighbours' crossovers: a band can't pass the one beside it.
            told = getCrossoverRoom (state).clipValue (juce::jlimit (lowestCrossover, highestCrossover, value));
            moveOnePoint (song, ref, told);
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

float getCurrentValue (const Song& song, const ParameterRef& ref, bool withModulation)
{
    const auto parameter = locateParameter (song, ref);
    return parameter.has_value() ? getCurrentValue (song, *parameter, withModulation) : 0.0f;
}

//==============================================================================
bool isDefaultValue (const devices::ParameterInfo& info, float value)
{
    // To a millionth of its size: a value back from a control's range can be a little off.
    return std::abs (value - info.defaultValue) <= 1.0e-6f * std::max (1.0f, std::abs (info.defaultValue));
}

void keepValueSetByHand (juce::ValueTree element, const devices::ParameterInfo& info, float value, juce::UndoManager* undo)
{
    const juce::Identifier parameter (toString (info.id));

    if (! isDefaultValue (info, value))
    {
        element.getOrCreateChildWithName (schema::VALUES, undo).setProperty (parameter, value, undo);
        return;
    }

    if (auto values = element.getChildWithName (schema::VALUES); values.isValid())
    {
        values.removeProperty (parameter, undo);

        if (values.getNumProperties() == 0 && values.getNumChildren() == 0)
            element.removeChild (values, undo);
    }
}
} // namespace phazite
