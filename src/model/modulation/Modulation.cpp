// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Modulation.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/Json.h"
#include "model/base/Numbers.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace phazite
{
namespace
{
    juce::UndoManager* getUndo (Song& song)
    {
        return &song.getUndoManager();
    }

    //==============================================================================
    // The elements, for the writers alone, which write them until they move onto the values (roadmap step S5.20,
    // modulation's third piece): every modulator's and macro's element (the tracks', the master's and the containers'); the
    // ID an element names; what a MODULATES names; a step sequencer's steps as its element keeps them.
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
            steps.push_back (juce::jlimit (-1.0f, 1.0f, readFloat (value))); // a version's are as anyone wrote them

        return steps;
    }

    // How many steps a new step sequencer's element plays (its numSteps set by hand, else its default).
    int getNumSteps (const juce::ValueTree& modulator)
    {
        constexpr auto kind = devices::ModulatorKind::steps;
        const auto& info = devices::getModulatorParameters (kind)[(size_t) devices::findModulatorParameter (kind, "numSteps")];
        return juce::jlimit (1, devices::Modulator::maxSteps, roundToNearestInt (readValueSetByHand (modulator, info), 1));
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

    //==============================================================================
    // The element of an owner of modulators or macros (ModulationValue.h: ModulatorPlace): a container's, the master's or a
    // track's; invalid for one the song hasn't.
    juce::ValueTree findOwnerState (const Song& song, const ModulatorPlace& owner)
    {
        if (owner.container.isValid())
        {
            const auto container = findDeviceState (song, owner.container);
            return container.hasType (schema::CONTAINER) ? container : juce::ValueTree();
        }

        return owner.onMaster ? findChainState (song, {}) : findTrackState (song, owner.track);
    }

    // A channel's own place: the master's, or a track's.
    ModulatorPlace getChannelPlace (const Song& song, TrackID track)
    {
        ModulatorPlace place;

        if (findChainState (song, track).hasType (schema::MASTER))
            place.onMaster = true;
        else
            place.track = track;

        return place;
    }
} // namespace

//==============================================================================
SongModulation getModulationValue (const Song& song)
{
    return readModulationValue (song.getState());
}

std::optional<ModulatorValue> getModulatorValue (const Song& song, ModulatorID id)
{
    const auto state = findModulatorState (song, id);
    return state.isValid() ? readModulatorValue (state, readModulatorPlace (state)) : std::nullopt;
}

std::vector<ModulatorValue> getOwnedModulators (const Song& song, const ModulatorPlace& owner)
{
    std::vector<ModulatorValue> modulators;

    for (const auto& child : findOwnerState (song, owner))
        if (child.hasType (schema::MODULATOR) || child.hasType (schema::MACRO))
            if (auto value = readModulatorValue (child, readModulatorPlace (child)))
                modulators.push_back (std::move (*value));

    return modulators;
}

std::vector<Modulation> findModulationsOf (const Song& song, const ParameterRef& parameter)
{
    std::vector<Modulation> modulations;

    for (const auto& [modulator, amount] : readModulationsOf (song.getState(), parameter))
        modulations.push_back ({ modulator, parameter, amount });

    return modulations;
}

bool isMacro (const Song& song, ModulatorID id)
{
    const auto value = getModulatorValue (song, id);
    return value.has_value() && value->isMacro();
}

std::vector<ModulatorID> getContainerMacros (const Song& song, DeviceID container)
{
    std::vector<ModulatorID> ids;

    if (container.isValid())
        for (const auto& macro : getOwnedModulators (song, { .container = container }))
            if (macro.isMacro())
                ids.push_back (macro.id);

    return ids;
}

bool hasModulations (const Song& song, TrackID track)
{
    if (! track.isValid())
        return false;

    const auto modulates = [] (const std::vector<ModulatorValue>& modulators)
    { return std::any_of (modulators.begin(), modulators.end(), [] (const ModulatorValue& each) { return ! each.modulations.empty(); }); };

    const auto channel = getChannelPlace (song, track);

    if (modulates (getOwnedModulators (song, channel)))
        return true;

    for (const auto& device : getDeviceStates (findChainState (song, track)))
        if (device.hasType (schema::CONTAINER))
            if (modulates (
                    getOwnedModulators (song, { .track = channel.track, .onMaster = channel.onMaster, .container = getDeviceID (device) })))
                return true;

    return false;
}

std::optional<ModulatorType> findModulatorType (const juce::String& name)
{
    for (size_t i = 0; i < std::size (schema::preset::modulatorTypeNames); ++i)
        if (name == schema::preset::modulatorTypeNames[i])
            return (ModulatorType) i;

    return {};
}

ModulatorType getModulatorType (const ModulatorValue& value)
{
    if (! value.kind.has_value())
        return ModulatorType::macro;

    switch (*value.kind)
    {
        case devices::ModulatorKind::envelopeFollower:
            return ModulatorType::envelopeFollower;
        case devices::ModulatorKind::steps:
            return ModulatorType::steps;
        case devices::ModulatorKind::random:
            return ModulatorType::random;
        case devices::ModulatorKind::breakpoints:
            return ModulatorType::breakpoints;
        case devices::ModulatorKind::noteFollower:
            return ModulatorType::noteFollower;
        case devices::ModulatorKind::lfo:
            break;
    }

    return ModulatorType::lfo;
}

bool isModulatorOn (const ModulatorValue& value)
{
    return value.isMacro() || ! value.off;
}

bool isBipolar (const ModulatorValue& value)
{
    if (! value.kind.has_value())
        return false;

    switch (*value.kind)
    {
        case devices::ModulatorKind::lfo:
        case devices::ModulatorKind::random:
        case devices::ModulatorKind::breakpoints:
            return value.getParameterValue (devices::findModulatorParameter (*value.kind, "bipolar")) >= 0.5f;
        case devices::ModulatorKind::steps:
            return true; // steps go from -1 to 1
        case devices::ModulatorKind::envelopeFollower:
        case devices::ModulatorKind::noteFollower:
            break;
    }

    return false;
}

int getNumSteps (const ModulatorValue& value)
{
    if (value.kind != devices::ModulatorKind::steps)
        return 1;

    const auto numSteps = value.getParameterValue (devices::findModulatorParameter (devices::ModulatorKind::steps, "numSteps"));
    return juce::jlimit (1, devices::Modulator::maxSteps, roundToNearestInt (numSteps, 1));
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

//==============================================================================
std::vector<ModulatorID> getModulators (const Song& song, TrackID track)
{
    std::vector<ModulatorID> modulators, macros;

    for (const auto& each : getOwnedModulators (song, getChannelPlace (song, track)))
        (each.isMacro() ? macros : modulators).push_back (each.id);

    modulators.insert (modulators.end(), macros.begin(), macros.end());
    return modulators;
}

std::optional<ModulatorType> getModulatorType (const Song& song, ModulatorID id)
{
    const auto value = getModulatorValue (song, id);
    return value.has_value() ? std::optional (getModulatorType (*value)) : std::nullopt;
}

TrackID getModulatorTrack (const Song& song, ModulatorID id)
{
    const auto value = getModulatorValue (song, id);

    if (! value.has_value())
        return {};

    // The master's by its own ID, as a track's (Schema.h: MASTER).
    return value->place.onMaster ? getTrackID (findChainState (song, {})) : value->place.track;
}

DeviceID getModulatorContainer (const Song& song, ModulatorID id)
{
    const auto value = getModulatorValue (song, id);
    return value.has_value() ? value->place.container : DeviceID();
}

juce::String getModulatorName (const Song& song, ModulatorID id)
{
    // Its element found once and read whole; its owner's others before it only by their kinds (readModulatorKind()), never
    // whole: a channel's slots ask each of its modulators' names at each refresh.
    const auto state = findModulatorState (song, id);
    const auto value = state.isValid() ? readModulatorValue (state, readModulatorPlace (state)) : std::nullopt;

    if (! value.has_value())
        return {};

    // A macro has a name of its own, "Macro 1" at first.
    if (value->isMacro() && value->name.isNotEmpty())
        return value->name;

    // Its place among its owner's of its kind.
    int number = 0;

    for (const auto& each : state.getParent())
    {
        if (value->isMacro() ? each.hasType (schema::MACRO) : readModulatorKind (each) == value->kind)
            ++number;

        if (each == state)
            break;
    }

    return getModulatorTypeName (getModulatorType (*value)) + " " + juce::String (number);
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

    // LFOs and random sources start going both ways, where their polarity's default has them going one way only. Step
    // sequencers and envelope followers start at full depth, their default, which the song keeps no value for.
    if (type == ModulatorType::lfo || type == ModulatorType::random)
    {
        const auto kind = type == ModulatorType::lfo ? devices::ModulatorKind::lfo : devices::ModulatorKind::random;
        const auto& polarity = devices::getModulatorParameters (kind)[(size_t) devices::findModulatorParameter (kind, "bipolar")];
        keepValueSetByHand (modulator, polarity, 1.0f, nullptr);
    }

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
    const auto value = getModulatorValue (song, id);
    return value.has_value() && isBipolar (*value);
}

bool isModulatorOn (const Song& song, ModulatorID id)
{
    const auto value = getModulatorValue (song, id);
    return value.has_value() && isModulatorOn (*value);
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
    if (! locateParameter (song, parameter).has_value())
        return {};

    return findModulationsOf (song, parameter);
}

std::vector<Modulation> getModulations (const Song& song, ModulatorID id)
{
    std::vector<Modulation> modulations;

    if (const auto value = getModulatorValue (song, id))
        for (const auto& modulation : value->modulations)
            if (locateParameter (song, modulation.parameter).has_value())
                modulations.push_back ({ id, modulation.parameter, modulation.amount });

    return modulations;
}

bool canModulate (const Song& song, ModulatorID id, const ParameterRef& parameter)
{
    const auto value = getModulatorValue (song, id);
    const auto found = value.has_value() ? locateParameter (song, parameter) : std::nullopt;

    // A modulator's own parameters and a macro's value are set by hand.
    if (! found.has_value() || found->source == ParameterSource::modulator || found->source == ParameterSource::macro)
        return false;

    // A container's macros modulate the devices in it, and its chains' volumes, pans and crossovers.
    if (const auto container = value->place.container; container.isValid())
    {
        if (found->source == ParameterSource::device || found->source == ParameterSource::plugin)
            return findContainerOf (song, parameter.device) == container;

        return (found->source == ParameterSource::chainVolume || found->source == ParameterSource::chainPan
                || found->source == ParameterSource::crossover)
               && getChainContainer (song, parameter.device) == container;
    }

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

void renameModulated (Song& song, const ParameterRef& from, const ParameterRef& to)
{
    auto* undo = getUndo (song);
    const auto device = juce::String ((juce::int64) to.device.getRaw());

    for (const auto& modulator : getModulatorStates (song))
    {
        for (auto modulates : modulator)
        {
            if (modulates.hasType (schema::MODULATES) && getModulatedParameter (modulates) == from)
            {
                modulates.setProperty (schema::modulatesDevice, device, undo);
                modulates.setProperty (schema::modulatesParameter, to.parameter, undo);
            }
        }
    }
}

void copyModulations (Song& song, const std::map<std::uint64_t, std::uint64_t>& copied)
{
    for (auto modulator : getModulatorStates (song))
    {
        std::vector<juce::ValueTree> copies;

        for (const auto& child : modulator)
        {
            if (! child.hasType (schema::MODULATES))
                continue;

            const auto parameter = getModulatedParameter (child);
            const auto found = copied.find (parameter.device.getRaw());

            if (found == copied.end())
                continue;

            if (const ParameterRef copiedParameter { DeviceID::fromRaw (found->second), parameter.parameter };
                ! findModulatesState (modulator, copiedParameter).isValid())
            {
                auto copy = child.createCopy();
                copy.setProperty (schema::modulatesDevice, juce::String ((juce::int64) found->second), nullptr);
                copies.push_back (copy);
            }
        }

        for (const auto& copy : copies)
            modulator.appendChild (copy, getUndo (song));
    }
}

void copyModulators (Song& song, TrackID from, TrackID to, const std::map<std::uint64_t, std::uint64_t>& copied)
{
    auto target = to.isValid() ? findChainState (song, to) : juce::ValueTree();
    std::vector<juce::ValueTree> copies;

    for (const auto& modulator : from.isValid() ? findChainState (song, from) : juce::ValueTree())
    {
        if (! (modulator.hasType (schema::MODULATOR) && canHoldModulators (target))
            && ! (modulator.hasType (schema::MACRO) && canHoldMacros (target)))
            continue;

        // As it is set, with what it modulates of the originals alone, which its copy modulates of their copies.
        auto copy = modulator.createCopy();

        for (auto i = copy.getNumChildren(); --i >= 0;)
        {
            auto child = copy.getChild (i);

            if (! child.hasType (schema::MODULATES))
                continue;

            if (const auto found = copied.find (getModulatedParameter (child).device.getRaw()); found != copied.end())
                child.setProperty (schema::modulatesDevice, juce::String ((juce::int64) found->second), nullptr);
            else
                copy.removeChild (i, nullptr);
        }

        if (copy.getChildWithName (schema::MODULATES).isValid())
        {
            copy.setProperty (schema::modulatorID, (juce::int64) song.makeID(), nullptr);
            copies.push_back (copy);
        }
    }

    for (const auto& copy : copies)
        target.appendChild (copy, getUndo (song));
}
} // namespace phazite
