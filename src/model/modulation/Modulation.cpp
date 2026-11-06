// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Modulation.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"

#include <algorithm>
#include <cmath>

namespace phazite
{
namespace
{
    juce::String toString (std::string_view text)
    {
        return juce::String (text.data(), text.size());
    }

    juce::UndoManager* getUndo (Song& song)
    {
        return &song.getUndoManager();
    }

    // A modulator's parameter's value set by hand, as its element keeps it (Schema.h: VALUES), else its default.
    float getValue (const juce::ValueTree& modulator, devices::ModulatorKind kind, std::string_view id)
    {
        const auto index = devices::findModulatorParameter (kind, id);

        if (index < 0)
            return 0.0f;

        const auto& info = devices::getModulatorParameters (kind)[(size_t) index];
        return (float) modulator.getChildWithName (schema::VALUES).getProperty (juce::Identifier (toString (info.id)), info.defaultValue);
    }

    // How many steps a step sequencer plays.
    int getNumSteps (const juce::ValueTree& modulator)
    {
        return juce::jlimit (1, devices::Modulator::maxSteps,
                             juce::roundToInt (getValue (modulator, devices::ModulatorKind::steps, "numSteps")));
    }

    // Whether a channel's element can hold modulators: a track's, a folder's or the master's; and macros: a track's or a
    // return's, which hold clips or take them in, not a folder's or the master's.
    bool canHoldModulators (const juce::ValueTree& channel)
    {
        return channel.hasType (schema::TRACK) || channel.hasType (schema::MASTER);
    }

    bool canHoldMacros (const juce::ValueTree& owner)
    {
        return (owner.hasType (schema::TRACK) && ! isFolderState (owner)) || owner.hasType (schema::CONTAINER);
    }

    // What a modulator modulates, by the parameter, in the song (Schema.h: MODULATES); invalid for none.
    juce::ValueTree findModulatesState (const juce::ValueTree& modulator, const ParameterRef& parameter)
    {
        for (const auto& child : modulator)
            if (child.hasType (schema::MODULATES) && getModulatedParameter (child) == parameter)
                return child;

        return {};
    }

    void writeSteps (juce::ValueTree modulator, std::vector<float> steps, juce::UndoManager* undo)
    {
        while (! steps.empty() && std::abs (steps.back()) < 1.0e-4f)
            steps.pop_back();

        juce::StringArray values;

        for (const auto step : steps)
            values.add (juce::String (std::round ((double) step * 1.0e4) / 1.0e4));

        if (values.isEmpty())
            modulator.removeProperty (schema::modulatorSteps, undo);
        else
            modulator.setProperty (schema::modulatorSteps, values.joinIntoString (" "), undo);
    }

    // A channel's element of a modulator's or a macro's: its parent's, or a container's macro's container's parent's.
    juce::ValueTree findOwnerChannel (const juce::ValueTree& modulator)
    {
        const auto parent = modulator.getParent();
        return parent.hasType (schema::CONTAINER) ? parent.getParent() : parent;
    }
} // namespace

//==============================================================================
std::optional<ModulatorType> findModulatorType (const juce::String& name)
{
    for (size_t i = 0; i < std::size (schema::preset::modulatorTypeNames); ++i)
        if (name == schema::preset::modulatorTypeNames[i])
            return (ModulatorType) i;

    return {};
}

std::optional<devices::ModulatorKind> getModulatorKind (const juce::ValueTree& state)
{
    const auto type = state.hasType (schema::MODULATOR) ? findModulatorType (state[schema::modulatorType].toString()) : std::nullopt;

    if (! type.has_value())
        return {};

    switch (*type)
    {
        case ModulatorType::envelopeFollower:
            return devices::ModulatorKind::envelopeFollower;
        case ModulatorType::steps:
            return devices::ModulatorKind::steps;
        case ModulatorType::random:
            return devices::ModulatorKind::random;
        case ModulatorType::breakpoints:
            return devices::ModulatorKind::breakpoints;
        case ModulatorType::noteFollower:
            return devices::ModulatorKind::noteFollower;
        case ModulatorType::lfo:
        case ModulatorType::macro:
            break;
    }

    return devices::ModulatorKind::lfo;
}

std::vector<juce::ValueTree> getModulatorStates (const Song& song)
{
    std::vector<juce::ValueTree> states;
    auto channels = getTrackStates (song);
    channels.push_back (findChainState (song, {}));

    const auto collect = [&states] (const juce::ValueTree& owner)
    {
        for (const auto& child : owner)
            if (child.hasType (schema::MODULATOR) || child.hasType (schema::MACRO))
                states.push_back (child);
    };

    for (const auto& channel : channels)
    {
        collect (channel);

        for (const auto& device : getDeviceStates (channel))
            if (device.hasType (schema::CONTAINER))
                collect (device);
    }

    return states;
}

juce::ValueTree findModulatorState (const Song& song, ModulatorID id)
{
    if (! id.isValid())
        return {};

    for (const auto& state : getModulatorStates (song))
        if (getModulatorID (state) == id)
            return state;

    return {};
}

ModulatorID getModulatorID (const juce::ValueTree& modulatorState)
{
    return ModulatorID::fromRaw ((std::uint64_t) (juce::int64) modulatorState[schema::modulatorID]);
}

ParameterRef getModulatedParameter (const juce::ValueTree& modulates)
{
    return { DeviceID::fromRaw ((std::uint64_t) (juce::int64) modulates[schema::modulatesDevice]),
             modulates[schema::modulatesParameter].toString() };
}

std::vector<float> getModulatorSteps (const juce::ValueTree& modulatorState)
{
    std::vector<float> steps;

    for (const auto& value : juce::StringArray::fromTokens (modulatorState[schema::modulatorSteps].toString(), " ", {}))
        steps.push_back (value.getFloatValue());

    return steps;
}

//==============================================================================
std::vector<ModulatorID> getModulators (const Song& song, TrackID track)
{
    std::vector<ModulatorID> modulators, macros;

    for (const auto& child : findChainState (song, track))
    {
        if (child.hasType (schema::MODULATOR) && getModulatorKind (child).has_value())
            modulators.push_back (getModulatorID (child));
        else if (child.hasType (schema::MACRO))
            macros.push_back (getModulatorID (child));
    }

    modulators.insert (modulators.end(), macros.begin(), macros.end());
    return modulators;
}

std::optional<ModulatorType> getModulatorType (const Song& song, ModulatorID id)
{
    const auto state = findModulatorState (song, id);

    if (state.hasType (schema::MACRO))
        return ModulatorType::macro;

    return state.hasType (schema::MODULATOR) ? findModulatorType (state[schema::modulatorType].toString()) : std::nullopt;
}

TrackID getModulatorTrack (const Song& song, ModulatorID id)
{
    const auto state = findModulatorState (song, id);
    return state.isValid() ? getTrackID (findOwnerChannel (state)) : TrackID();
}

DeviceID getModulatorContainer (const Song& song, ModulatorID id)
{
    const auto parent = findModulatorState (song, id).getParent();
    return parent.hasType (schema::CONTAINER) ? getDeviceID (parent) : DeviceID();
}

juce::String getModulatorName (const Song& song, ModulatorID id)
{
    const auto state = findModulatorState (song, id);

    // A macro has a name of its own, "Macro 1" at first.
    if (state.hasType (schema::MACRO) && state[schema::macroName].toString().isNotEmpty())
        return state[schema::macroName].toString();

    const auto type = getModulatorType (song, id);

    if (! type.has_value())
        return {};

    // Its place among its owner's of its kind.
    int number = 0;

    for (const auto& each : state.getParent())
    {
        if (each.getType() == state.getType()
            && (each.hasType (schema::MACRO) || each[schema::modulatorType] == state[schema::modulatorType]))
            ++number;

        if (each == state)
            break;
    }

    return getModulatorTypeName (*type) + " " + juce::String (number);
}

ModulatorID addModulator (Song& song, TrackID track, ModulatorType type)
{
    auto channel = track.isValid() ? findChainState (song, track) : juce::ValueTree();

    if (type == ModulatorType::macro)
    {
        if (! channel.hasType (schema::TRACK) || ! canHoldMacros (channel))
            return {};

        int count = 0;

        for (const auto& child : channel)
            if (child.hasType (schema::MACRO))
                ++count;

        return addMacro (song, channel, "Macro " + juce::String (count + 1));
    }

    if (! canHoldModulators (channel))
        return {};

    const auto id = ModulatorID::fromRaw (song.makeID());
    juce::ValueTree modulator (schema::MODULATOR);
    modulator.setProperty (schema::modulatorID, (juce::int64) id.getRaw(), nullptr);
    modulator.setProperty (schema::modulatorType, schema::preset::modulatorTypeNames[(size_t) type], nullptr);

    // LFOs and random sources start going both ways, and step sequencers and envelope followers at full depth, where their
    // parameters' defaults would have them going one way only, or moving nothing.
    juce::ValueTree values (schema::VALUES);

    if (type == ModulatorType::lfo || type == ModulatorType::random)
        values.setProperty ("bipolar", 1.0f, nullptr);
    else if (type == ModulatorType::envelopeFollower || type == ModulatorType::steps)
        values.setProperty ("depth", 1.0f, nullptr);

    if (values.getNumProperties() > 0)
        modulator.appendChild (values, nullptr);

    // A step sequencer's steps rise across as many steps as it starts with.
    if (type == ModulatorType::steps)
    {
        const auto numSteps = getNumSteps (modulator);
        std::vector<float> steps;

        for (int i = 0; i < numSteps; ++i)
            steps.push_back (numSteps > 1 ? -1.0f + 2.0f * (float) i / (float) (numSteps - 1) : 0.0f);

        writeSteps (modulator, steps, nullptr);
    }

    channel.appendChild (modulator, getUndo (song));
    return id;
}

ModulatorID addMacro (Song& song, juce::ValueTree owner, const juce::String& name)
{
    if (! canHoldMacros (owner) || ! owner.isAChildOf (song.getState()))
        return {};

    // Its value is 0 while its element has none (Schema.h: macroValue), so that redoing the step puts it back at 0 too.
    const auto id = ModulatorID::fromRaw (song.makeID());
    juce::ValueTree macro (schema::MACRO);
    macro.setProperty (schema::modulatorID, (juce::int64) id.getRaw(), nullptr);
    macro.setProperty (schema::macroName, name, nullptr);
    owner.appendChild (macro, getUndo (song));
    return id;
}

void renameMacro (Song& song, ModulatorID id, const juce::String& name)
{
    const auto trimmed = name.trim();

    if (auto state = findModulatorState (song, id); state.hasType (schema::MACRO) && trimmed.isNotEmpty())
        state.setProperty (schema::macroName, trimmed, getUndo (song));
}

void removeModulator (Song& song, ModulatorID id)
{
    // With what it modulates (its MODULATES).
    if (auto state = findModulatorState (song, id); state.isValid())
        state.getParent().removeChild (state, getUndo (song));
}

bool isBipolar (const Song& song, ModulatorID id)
{
    const auto state = findModulatorState (song, id);
    const auto kind = getModulatorKind (state);

    if (! kind.has_value())
        return false;

    switch (*kind)
    {
        case devices::ModulatorKind::lfo:
        case devices::ModulatorKind::random:
        case devices::ModulatorKind::breakpoints:
            return getValue (state, *kind, "bipolar") >= 0.5f;
        case devices::ModulatorKind::steps:
            return true; // steps go from -1 to 1
        case devices::ModulatorKind::envelopeFollower:
        case devices::ModulatorKind::noteFollower:
            break;
    }

    return false;
}

bool isModulatorOn (const Song& song, ModulatorID id)
{
    const auto state = findModulatorState (song, id);
    return state.hasType (schema::MACRO) || (state.hasType (schema::MODULATOR) && ! (bool) state[schema::deviceOff]);
}

void setModulatorOn (Song& song, ModulatorID id, bool on)
{
    auto state = findModulatorState (song, id);

    if (! state.hasType (schema::MODULATOR) || isModulatorOn (song, id) == on)
        return;

    if (on)
        state.removeProperty (schema::deviceOff, getUndo (song));
    else
        state.setProperty (schema::deviceOff, true, getUndo (song));
}

void setModulatorStep (Song& song, ModulatorID id, int index, float value)
{
    auto state = findModulatorState (song, id);

    if (! state.hasType (schema::MODULATOR) || ! juce::isPositiveAndBelow (index, devices::Modulator::maxSteps))
        return;

    auto steps = getModulatorSteps (state);
    steps.resize (std::max (steps.size(), (size_t) index + 1), 0.0f);
    steps[(size_t) index] = juce::jlimit (-1.0f, 1.0f, value);
    writeSteps (state, steps, getUndo (song));
}

//==============================================================================
std::vector<Modulation> getModulations (const Song& song, const ParameterRef& parameter)
{
    std::vector<Modulation> modulations;

    if (! locateParameter (song, parameter).has_value())
        return modulations;

    for (const auto& state : getModulatorStates (song))
        for (const auto& child : state)
            if (child.hasType (schema::MODULATES) && getModulatedParameter (child) == parameter)
                modulations.push_back ({ getModulatorID (state), parameter, (float) child[schema::modulatesAmount] });

    return modulations;
}

std::vector<Modulation> getModulations (const Song& song, ModulatorID id)
{
    std::vector<Modulation> modulations;

    for (const auto& child : findModulatorState (song, id))
        if (const auto parameter = getModulatedParameter (child);
            child.hasType (schema::MODULATES) && locateParameter (song, parameter).has_value())
            modulations.push_back ({ id, parameter, (float) child[schema::modulatesAmount] });

    return modulations;
}

bool canModulate (const Song& song, ModulatorID id, const ParameterRef& parameter)
{
    const auto state = findModulatorState (song, id);
    const auto found = state.isValid() ? locateParameter (song, parameter) : std::nullopt;

    // A modulator's own parameters and a macro's value are set by hand.
    if (! found.has_value() || found->source == ParameterSource::modulator || found->source == ParameterSource::macro)
        return false;

    // A container's macros modulate the devices in it.
    if (const auto owner = state.getParent(); owner.hasType (schema::CONTAINER))
        return (found->source == ParameterSource::device || found->source == ParameterSource::plugin)
               && findContainerOf (song, parameter.device) == getDeviceID (owner);

    const auto track = getModulatorTrack (song, id);
    return track.isValid() && getParameterTrack (song, parameter) == track;
}

void setModulation (Song& song, ModulatorID id, const ParameterRef& parameter, float amount)
{
    auto state = findModulatorState (song, id);

    if (! state.isValid() || ! parameter.isValid())
        return;

    // By the parameter's device's ID as a curve names it, and its own ID.
    auto* undo = getUndo (song);
    auto modulates = findModulatesState (state, parameter);

    if (! modulates.isValid())
    {
        modulates = juce::ValueTree (schema::MODULATES);
        modulates.setProperty (schema::modulatesDevice, juce::String ((juce::int64) parameter.device.getRaw()), nullptr);
        modulates.setProperty (schema::modulatesParameter, parameter.parameter, nullptr);
        state.appendChild (modulates, undo);
    }

    modulates.setProperty (schema::modulatesAmount, std::round ((double) juce::jlimit (-1.0f, 1.0f, amount) * 1.0e4) / 1.0e4, undo);
}

void removeModulation (Song& song, ModulatorID id, const ParameterRef& parameter)
{
    if (auto modulates = findModulatesState (findModulatorState (song, id), parameter); modulates.isValid())
        modulates.getParent().removeChild (modulates, getUndo (song));
}
} // namespace phazite
