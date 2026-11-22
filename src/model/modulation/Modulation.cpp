// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Modulation.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/Numbers.h"
#include "model/base/internal/SavedValues.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/versions/internal/AreaClashes.h"
#include "model/versions/internal/SongMerge.h"
#include "model/versions/internal/SongPieces.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    // The song's tree written as the song is written now: in the step in progress, or outside the history.
    juce::UndoManager* getTreeUndo (Song& song)
    {
        return song.isWritingOutsideHistory() ? nullptr : &song.getUndoManager();
    }

    //==============================================================================
    // The elements, for the setters alone (setModulator(), eraseModulator(), setModulatorOrder()) and the rename of a
    // modulation in place (renameModulated()), which write them while the song's tree is modulation's storage (roadmap step
    // S5.20, until modulation's switch): every modulator's and macro's element (the tracks', the master's and the
    // containers'); the ID an element names; what a MODULATES names.
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
        return ModulatorID::fromRaw (readID (modulatorState[schema::modulatorID]));
    }

    // As the song's readers read it (ModulationValue.h: readModulatorValue()).
    ParameterRef getModulatedParameter (const juce::ValueTree& modulates)
    {
        return { DeviceID::fromRaw (readID (modulates[schema::modulatesDevice])), modulates[schema::modulatesParameter].toString() };
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

    bool isModulatorElement (const juce::ValueTree& element)
    {
        return element.hasType (schema::MODULATOR) || element.hasType (schema::MACRO);
    }

    // Whether two places have one owner (ModulationValue.h: ModulatorPlace): the same container where either names one,
    // else the same channel.
    bool isSameOwner (const ModulatorPlace& a, const ModulatorPlace& b)
    {
        if (a.container.isValid() || b.container.isValid())
            return a.container == b.container;

        return a.onMaster == b.onMaster && (a.onMaster || a.track == b.track);
    }

    // Where a place in an owner's list (from 0) is among its element's children: before its modulator or macro at that
    // place; -1 (after all its children, as Phazite's writers always added one) for -1 and a place past its last.
    int findChildIndex (const juce::ValueTree& owner, int index)
    {
        if (index < 0)
            return -1;

        int place = 0;

        for (int i = 0; i < owner.getNumChildren(); ++i)
            if (isModulatorElement (owner.getChild (i)) && place++ == index)
                return i;

        return -1;
    }

    // An amount as Phazite writes one: to a ten-thousandth.
    double writeAmount (float amount)
    {
        return std::round ((double) juce::jlimit (-1.0f, 1.0f, amount) * 1.0e4) / 1.0e4;
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

    //==============================================================================
    // A modulator's element made to hold the next value from the one it holds (held, as readModulatorValue() reads it), by
    // what differs, as Phazite's writers wrote each part: its values set by hand as keepValueSetByHand() keeps them (one at
    // its default taken away, the last taken away with its VALUES), by their parameters' names, so that another kind (a
    // merge's choice of another type) keeps what its parameters of those names held. Whether it wrote.
    bool writeValuesLike (juce::ValueTree element, const ModulatorValue& held, const ModulatorValue& next, juce::UndoManager* undo)
    {
        const auto heldParameters = devices::getModulatorParameters (*held.kind);
        const auto nextParameters = devices::getModulatorParameters (*next.kind);
        auto wrote = false;

        // What it holds that the next holds none of, taken away.
        for (const auto& [index, was] : held.values)
        {
            if (index >= heldParameters.size())
                continue;

            const auto& info = heldParameters[index];

            if (const auto now = devices::findModulatorParameter (*next.kind, info.id);
                now < 0 || next.values.find ((std::uint32_t) now) == nullptr)
            {
                keepValueSetByHand (element, info, info.defaultValue, undo);
                wrote = true;
            }
        }

        // What the next holds otherwise than it does, or that it holds none of, set.
        for (const auto& [index, value] : next.values)
        {
            if (index >= nextParameters.size())
                continue;

            const auto& info = nextParameters[index];
            const auto was = devices::findModulatorParameter (*held.kind, info.id);
            const auto* heldValue = was >= 0 ? held.values.find ((std::uint32_t) was) : nullptr;

            if (heldValue == nullptr || ! juce::exactlyEqual (*heldValue, value))
            {
                keepValueSetByHand (element, info, value, undo);
                wrote = true;
            }
        }

        return wrote;
    }

    bool isSameSteps (const immer::flex_vector<float>& a, const immer::flex_vector<float>& b)
    {
        if (a.size() != b.size())
            return false;

        for (size_t i = 0; i < a.size(); ++i)
            if (! juce::exactlyEqual (a[i], b[i]))
                return false;

        return true;
    }

    // What it modulates made the next value's, as Phazite's writers wrote it: each MODULATES of a parameter the next modulates
    // nothing of taken out; each the next keeps given its amount where it would write another text (SongText.h:
    // isSameValue()); each the next modulates that it doesn't, made whole and added after the others (the only order an edit
    // makes); then all of them in the next's order, which only a version's or a merge's next can make another. Whether it
    // wrote.
    bool writeModulationsLike (juce::ValueTree element, const ModulatorValue& next, juce::UndoManager* undo)
    {
        auto wrote = false;

        for (int i = 0; i < element.getNumChildren();)
        {
            if (const auto child = element.getChild (i);
                child.hasType (schema::MODULATES) && next.findModulation (getModulatedParameter (child)) == nullptr)
            {
                element.removeChild (i, undo);
                wrote = true;
            }
            else
            {
                ++i;
            }
        }

        std::vector<ParameterRef> held;

        for (auto child : element)
        {
            if (! child.hasType (schema::MODULATES))
                continue;

            const auto parameter = getModulatedParameter (child);
            const auto* modulation = next.findModulation (parameter);
            held.push_back (parameter);

            if (modulation == nullptr)
                continue;

            const juce::var amount (writeAmount (modulation->amount));

            if (const auto* written = child.getPropertyPointer (schema::modulatesAmount);
                written == nullptr || ! isSameValue (*written, amount))
            {
                child.setProperty (schema::modulatesAmount, amount, undo);
                wrote = true;
            }
        }

        for (const auto& modulation : next.modulations)
        {
            if (std::find (held.begin(), held.end(), modulation.parameter) != held.end())
                continue;

            // By the parameter's device's ID as a curve names it, and its own ID.
            juce::ValueTree modulates (schema::MODULATES);
            modulates.setProperty (schema::modulatesDevice, juce::String ((juce::int64) modulation.parameter.device.getRaw()), nullptr);
            modulates.setProperty (schema::modulatesParameter, modulation.parameter.parameter, nullptr);
            modulates.setProperty (schema::modulatesAmount, writeAmount (modulation.amount), nullptr);
            element.appendChild (modulates, undo);
            wrote = true;
        }

        // Each moved to the next place a MODULATES is at (the other children stay where they are).
        int place = 0;

        for (const auto& modulation : next.modulations)
        {
            int index = -1;

            for (int i = 0; i < element.getNumChildren() && index < 0; ++i)
                if (const auto child = element.getChild (i);
                    child.hasType (schema::MODULATES) && getModulatedParameter (child) == modulation.parameter)
                    index = i;

            if (index < place)
                continue;

            while (place < element.getNumChildren() && ! element.getChild (place).hasType (schema::MODULATES))
                ++place;

            if (index != place)
            {
                element.moveChild (index, place, undo);
                wrote = true;
            }

            ++place;
        }

        return wrote;
    }

    // A macro's value as Phazite's writers write it: only while not 0.
    void writeMacroValue (juce::ValueTree macro, float value, juce::UndoManager* undo)
    {
        if (juce::exactlyEqual (value, 0.0f))
            macro.removeProperty (schema::macroValue, undo);
        else
            macro.setProperty (schema::macroValue, value, undo);
    }

    // A modulator's or a macro's element made to hold the next value from the one it holds (held, of its kind: a macro's
    // or a modulator's), by what differs, each property by its name as Phazite's writers wrote it: a macro's name always,
    // its value only while not 0 (writeMacroValue()); a modulator's type, its switch only while off, its values set by hand
    // (writeValuesLike()), its steps only while it has one (writeSteps()); then what it modulates (writeModulationsLike()).
    // A list or a map the next holds as the held one does (the same, not looked inside: an edit of another part, which
    // copied the value read) is left as it is written. Whether it wrote.
    bool writeModulatorLike (juce::ValueTree element, const ModulatorValue& held, const ModulatorValue& next, juce::UndoManager* undo)
    {
        auto wrote = false;

        if (next.isMacro())
        {
            if (held.name != next.name)
            {
                element.setProperty (schema::macroName, next.name, undo);
                wrote = true;
            }

            if (! juce::exactlyEqual (held.value, next.value))
            {
                writeMacroValue (element, next.value, undo);
                wrote = true;
            }
        }
        else
        {
            if (held.kind != next.kind)
            {
                element.setProperty (schema::modulatorType, schema::preset::modulatorTypeNames[(size_t) *next.kind], undo);
                wrote = true;
            }

            if (held.off != next.off)
            {
                if (next.off)
                    element.setProperty (schema::deviceOff, true, undo);
                else
                    element.removeProperty (schema::deviceOff, undo);

                wrote = true;
            }

            if (held.kind != next.kind || held.values.identity() != next.values.identity())
                wrote = writeValuesLike (element, held, next, undo) || wrote;

            if (held.steps.identity() != next.steps.identity() && ! isSameSteps (held.steps, next.steps))
            {
                writeSteps (element, std::vector<float> (next.steps.begin(), next.steps.end()), undo);
                wrote = true;
            }
        }

        if (held.modulations.identity() == next.modulations.identity())
            return wrote;

        return writeModulationsLike (element, next, undo) || wrote;
    }

    // An element's step in a merge's clash's path (model/versions/SongMerge.cpp): its type and key ("MODULATOR:93").
    juce::String getPathStep (const juce::ValueTree& element)
    {
        return element.getType().toString() + ":" + schema::getElementKey (element);
    }

    // The place an owner's element in the song gives what it holds: a track's or the master's own, or a container's on one;
    // none for another element.
    std::optional<ModulatorPlace> readOwnerPlace (const juce::ValueTree& owner)
    {
        if (owner.hasType (schema::MASTER))
            return ModulatorPlace { .onMaster = true };

        if (owner.hasType (schema::TRACK))
            return ModulatorPlace { .track = TrackID::fromRaw (readID (owner[schema::trackID])) };

        if (! owner.hasType (schema::CONTAINER))
            return std::nullopt;

        auto place = readOwnerPlace (owner.getParent());

        if (! place.has_value() || place->container.isValid())
            return std::nullopt;

        place->container = DeviceID::fromRaw (readID (owner[schema::deviceID]));
        return place;
    }

    // Every modulator and macro of the song, in the song's order: each channel's (its tracks', in their order, then the
    // master's), its own and then its containers', each owner's in its order.
    std::vector<ModulatorValue> getEveryModulator (const Song& song)
    {
        std::vector<ModulatorValue> every;
        const auto take = [&song, &every] (const ModulatorPlace& owner)
        {
            for (auto& value : getOwnedModulators (song, owner))
                every.push_back (std::move (value));
        };

        auto channels = getTrackStates (song);
        channels.push_back (findChainState (song, {}));

        for (const auto& channel : channels)
        {
            const auto place =
                channel.hasType (schema::MASTER) ? ModulatorPlace { .onMaster = true } : ModulatorPlace { .track = getTrackID (channel) };
            take (place);

            for (const auto& device : getDeviceStates (channel))
                if (device.hasType (schema::CONTAINER))
                    take ({ .track = place.track, .onMaster = place.onMaster, .container = getDeviceID (device) });
        }

        return every;
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

//==============================================================================
bool setModulator (Song& song, const ModulatorValue& value, int index)
{
    // Where its place says, whatever it is (what a file holds and Phazite never makes, a macro on a folder, stays as it is: a
    // version of it, a merge's choice); nothing for a place the song hasn't.
    auto owner = findOwnerState (song, value.place);

    if (! value.id.isValid() || ! owner.isValid())
        return false;

    auto* undo = getTreeUndo (song);
    auto element = findModulatorState (song, value.id);
    const auto held = element.isValid() ? readModulatorValue (element, readModulatorPlace (element)) : std::nullopt;

    // One the song hasn't (or holds as what no reader reads, or as a macro under a modulator's ID or the other way round):
    // its element made whole, then added at its place, in one action.
    if (! held.has_value() || held->isMacro() != value.isMacro())
    {
        if (element.isValid())
            element.getParent().removeChild (element, undo);

        owner.addChild (writeModulatorElement (value), findChildIndex (owner, index), undo);
        return true;
    }

    // One that has another owner: moved to the place given in the new owner's list (a version's), then made the value.
    auto wrote = false;

    if (! isSameOwner (held->place, value.place))
    {
        element.getParent().removeChild (element, undo);
        owner.addChild (element, findChildIndex (owner, index), undo);
        wrote = true;
    }

    return writeModulatorLike (element, *held, value, undo) || wrote;
}

bool eraseModulator (Song& song, ModulatorID id)
{
    auto element = findModulatorState (song, id);

    if (! element.isValid())
        return false;

    element.getParent().removeChild (element, getTreeUndo (song));
    return true;
}

bool setModulatorOrder (Song& song, const ModulatorPlace& owner, const immer::flex_vector<ModulatorID>& order)
{
    // The places its modulators and macros are at among the owner's children, and which is at each.
    auto element = findOwnerState (song, owner);
    std::vector<int> places;
    std::vector<ModulatorID> held;

    for (int i = 0; i < element.getNumChildren(); ++i)
    {
        if (const auto child = element.getChild (i); isModulatorElement (child))
        {
            places.push_back (i);
            held.push_back (getModulatorID (child));
        }
    }

    // Those named, in their order (each once), then the others in theirs.
    std::vector<ModulatorID> wanted;
    const auto isWanted = [&wanted] (ModulatorID id) { return std::find (wanted.begin(), wanted.end(), id) != wanted.end(); };

    for (const auto id : order)
        if (std::find (held.begin(), held.end(), id) != held.end() && ! isWanted (id))
            wanted.push_back (id);

    for (const auto id : held)
        if (! isWanted (id))
            wanted.push_back (id);

    if (wanted == held)
        return false;

    // Each put at its place by changing places with the one there (two moves, which leave what is between them where it was),
    // so that the owner's other children (its devices, lanes and sends) stay at theirs.
    auto* undo = getTreeUndo (song);

    for (size_t k = 0; k < wanted.size(); ++k)
    {
        if (held[k] == wanted[k])
            continue;

        const auto j = (size_t) std::distance (held.begin(), std::find (held.begin() + (std::ptrdiff_t) k, held.end(), wanted[k]));
        element.moveChild (places[j], places[k], undo);
        element.moveChild (places[k] + 1, places[j], undo);
        std::swap (held[k], held[j]);
    }

    return true;
}

//==============================================================================
ModulatorID addModulator (Song& song, TrackID track, ModulatorType type)
{
    const auto channel = track.isValid() ? findChainState (song, track) : juce::ValueTree();

    if (type == ModulatorType::macro)
    {
        if (! channel.hasType (schema::TRACK) || ! canHoldMacros (channel))
            return {};

        const auto owned = getOwnedModulators (song, getChannelPlace (song, track));
        const auto count = std::count_if (owned.begin(), owned.end(), [] (const ModulatorValue& each) { return each.isMacro(); });
        return addMacro (song, track, "Macro " + juce::String ((int) count + 1));
    }

    if (! canHoldModulators (channel))
        return {};

    ModulatorValue value;
    value.id = ModulatorID::fromRaw (song.makeID());
    value.place = getChannelPlace (song, track);
    value.kind = (devices::ModulatorKind) type; // ModulatorType's order is the kinds' (ModulationValue.cpp)

    // LFOs and random sources start going both ways, where their polarity's default has them going one way only. Step
    // sequencers and envelope followers start at full depth, their default, which the song keeps no value for.
    if (type == ModulatorType::lfo || type == ModulatorType::random)
    {
        const auto index = devices::findModulatorParameter (*value.kind, "bipolar");

        if (! isDefaultValue (devices::getModulatorParameters (*value.kind)[(size_t) index], 1.0f))
            value.values = value.values.set ((std::uint32_t) index, 1.0f);
    }

    // A step sequencer's steps rise across as many steps as it starts with.
    if (type == ModulatorType::steps)
    {
        const auto numSteps = getNumSteps (value);
        auto steps = value.steps.transient();

        for (int i = 0; i < numSteps; ++i)
            steps.push_back (numSteps > 1 ? -1.0f + 2.0f * (float) i / (float) (numSteps - 1) : 0.0f);

        value.steps = steps.persistent();
    }

    setModulator (song, value);
    return value.id;
}

ModulatorID addMacro (Song& song, TrackID track, const juce::String& name)
{
    if (const auto channel = track.isValid() ? findChainState (song, track) : juce::ValueTree();
        ! channel.hasType (schema::TRACK) || ! canHoldMacros (channel))
        return {};

    // Its value is 0 while its element has none (Schema.h: macroValue), so that redoing the step puts it back at 0 too.
    ModulatorValue value;
    value.id = ModulatorID::fromRaw (song.makeID());
    value.place = getChannelPlace (song, track);
    value.name = name;
    setModulator (song, value);
    return value.id;
}

void renameMacro (Song& song, ModulatorID id, const juce::String& name)
{
    const auto trimmed = name.trim();

    if (auto value = getModulatorValue (song, id); value.has_value() && value->isMacro() && trimmed.isNotEmpty())
    {
        value->name = trimmed;
        setModulator (song, *value);
    }
}

void removeModulator (Song& song, ModulatorID id)
{
    // With what it modulates; and what names it goes with it (a MIDI link of a macro's value or of a modulator's parameter),
    // as with every other removal (known bug 241).
    if (eraseModulator (song, id))
        forgetWhatNamesTheGone (song);
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
    if (auto value = getModulatorValue (song, id); value.has_value() && ! value->isMacro() && isModulatorOn (*value) != on)
    {
        value->off = ! on;
        setModulator (song, *value);
    }
}

void setModulatorStep (Song& song, ModulatorID id, int index, float step)
{
    auto value = getModulatorValue (song, id);

    if (! value.has_value() || value->isMacro() || ! juce::isPositiveAndBelow (index, devices::Modulator::maxSteps))
        return;

    std::vector<float> steps (value->steps.begin(), value->steps.end());
    steps.resize (std::max (steps.size(), (size_t) index + 1), 0.0f);
    steps[(size_t) index] = juce::jlimit (-1.0f, 1.0f, step);

    // Without the zeros at the end, as its element writes them (writeSteps()).
    while (! steps.empty() && std::abs (steps.back()) < 1.0e-4f)
        steps.pop_back();

    value->steps = immer::flex_vector<float> (steps.begin(), steps.end());
    setModulator (song, *value);
}

// A control's drag sets these at the mouse's rate: its element found once, and the one value read and written (setModulator()'s
// write by what differs, of the one part an edit of the value changes), nothing else of it read whole.
void setModulatorValueSetByHand (Song& song, ModulatorID id, const devices::ParameterInfo& info, float set)
{
    auto element = findModulatorState (song, id);

    if (const auto kind = readModulatorKind (element); ! kind.has_value() || devices::findModulatorParameter (*kind, info.id) < 0)
        return;

    // None at its default, as keepValueSetByHand() keeps it (model/devices/internal/Parameters.h); nothing where it holds that.
    const auto held = readModulatorValueSetByHand (element, info);
    const auto next = isDefaultValue (info, set) ? std::nullopt : std::optional<float> (set);

    if (held.has_value() == next.has_value() && (! held.has_value() || juce::exactlyEqual (*held, *next)))
        return;

    keepValueSetByHand (element, info, set, getTreeUndo (song));
}

void setMacroValue (Song& song, ModulatorID id, float set)
{
    if (auto element = findModulatorState (song, id);
        element.hasType (schema::MACRO) && ! juce::exactlyEqual (readMacroValue (element), set))
        writeMacroValue (element, set, getTreeUndo (song));
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
    auto value = getModulatorValue (song, id);

    if (! value.has_value() || ! parameter.isValid())
        return;

    // A modulation it has keeps its place; one it hasn't goes after the others.
    const ModulationEntry entry { .parameter = parameter, .amount = (float) writeAmount (amount) };
    auto modulations = value->modulations.push_back (entry);

    for (size_t i = 0; i < value->modulations.size(); ++i)
    {
        if (value->modulations[i].parameter == parameter)
        {
            modulations = value->modulations.set (i, entry);
            break;
        }
    }

    value->modulations = modulations;
    setModulator (song, *value);
}

void removeModulation (Song& song, ModulatorID id, const ParameterRef& parameter)
{
    auto value = getModulatorValue (song, id);

    if (! value.has_value() || value->findModulation (parameter) == nullptr)
        return;

    auto kept = immer::flex_vector<ModulationEntry>().transient();

    for (const auto& modulation : value->modulations)
        if (! (modulation.parameter == parameter))
            kept.push_back (modulation);

    value->modulations = kept.persistent();
    setModulator (song, *value);
}

int forgetModulationsOf (Song& song, const std::function<bool (DeviceID)>& gone)
{
    // Those that modulate what is gone found without reading any modulator whole (as findModulationsOf() reads, while the
    // tree is the storage: every removal of a device asks), then each of them read and set.
    std::vector<ModulatorID> modulating;

    for (const auto& modulator : getModulatorStates (song))
    {
        for (const auto& child : modulator)
        {
            if (child.hasType (schema::MODULATES) && gone (getModulatedParameter (child).device))
            {
                modulating.push_back (getModulatorID (modulator));
                break;
            }
        }
    }

    auto forgotten = 0;

    for (const auto id : modulating)
    {
        auto value = getModulatorValue (song, id);

        if (! value.has_value())
            continue;

        auto kept = immer::flex_vector<ModulationEntry>().transient();

        for (const auto& modulation : value->modulations)
            if (! gone (modulation.parameter.device))
                kept.push_back (modulation);

        forgotten += (int) (value->modulations.size() - kept.size());
        value->modulations = kept.persistent();
        setModulator (song, *value);
    }

    return forgotten;
}

void renameModulated (Song& song, const ParameterRef& from, const ParameterRef& to)
{
    // In place, each element's device and parameter written (a modulation set anew would go after the others).
    auto* undo = getTreeUndo (song);
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
    for (auto value : getEveryModulator (song))
    {
        // Its modulations of the originals, each of its copy too where it modulates none of that copy's parameter yet, after
        // the others, in their order.
        auto modulations = value.modulations;

        for (const auto& modulation : value.modulations)
        {
            const auto found = copied.find (modulation.parameter.device.getRaw());

            if (found == copied.end())
                continue;

            if (const ParameterRef copy { DeviceID::fromRaw (found->second), modulation.parameter.parameter };
                value.findModulation (copy) == nullptr)
                modulations = modulations.push_back ({ .parameter = copy, .amount = modulation.amount });
        }

        if (modulations.size() != value.modulations.size())
        {
            value.modulations = modulations;
            setModulator (song, value);
        }
    }
}

void copyModulators (Song& song, TrackID from, TrackID to, CopiedIDs& copied)
{
    const auto target = to.isValid() ? findChainState (song, to) : juce::ValueTree();

    if (! from.isValid() || ! target.isValid())
        return;

    const auto place = getChannelPlace (song, to);
    std::vector<ModulatorValue> copies;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> copiedModulators;

    for (const auto& modulator : getOwnedModulators (song, getChannelPlace (song, from)))
    {
        if (! (modulator.isMacro() ? canHoldMacros (target) : canHoldModulators (target)))
            continue;

        // As it is set, with what it modulates of the originals alone, which its copy modulates of their copies.
        auto modulations = immer::flex_vector<ModulationEntry>().transient();

        for (const auto& modulation : modulator.modulations)
            if (const auto found = copied.find (modulation.parameter.device.getRaw()); found != copied.end())
                modulations.push_back (
                    { .parameter = { DeviceID::fromRaw (found->second), modulation.parameter.parameter }, .amount = modulation.amount });

        if (modulations.size() == 0)
            continue;

        auto copy = modulator;
        copy.id = ModulatorID::fromRaw (song.makeID());
        copy.place = place;
        copy.modulations = modulations.persistent();
        copiedModulators.emplace_back (modulator.id.getRaw(), copy.id.getRaw());
        copies.push_back (std::move (copy));
    }

    // Each copy's ID among the copied, once all of them are worked out, for what names a modulator's or a macro's parameter
    // (a macro's value's automation: Automation.h, copyCurves(), after this).
    for (const auto& [original, copy] : copiedModulators)
        copied[original] = copy;

    for (const auto& copy : copies)
        setModulator (song, copy);
}

void copyModulation (Song& song, CopiedIDs& copied)
{
    // The owners that hold modulators or macros, found by their elements' places alone, none read whole: a duplicated
    // track's copy holds its own while the song's tree is modulation's storage, so that nothing is read for it.
    std::map<std::uint64_t, ModulatorPlace> channels, containers;

    for (const auto& element : getModulatorStates (song))
    {
        if (const auto place = readModulatorPlace (element); place.container.isValid())
            containers.emplace (place.container.getRaw(), place);
        else if (! place.onMaster)
            channels.emplace (place.track.getRaw(), place);
    }

    // The owners copied whose copies hold none: each original's own (a track's, a container's), its copy's place (a
    // container's copy on its channel's copy, or on the channel itself).
    std::vector<std::pair<ModulatorPlace, std::vector<ModulatorValue>>> owners;

    for (const auto& [original, copy] : copied)
    {
        if (const auto found = channels.find (original); found != channels.end() && channels.count (copy) == 0)
            owners.emplace_back (ModulatorPlace { .track = TrackID::fromRaw (copy) }, getOwnedModulators (song, found->second));

        if (const auto found = containers.find (original); found != containers.end() && containers.count (copy) == 0)
        {
            auto place = found->second;
            place.container = DeviceID::fromRaw (copy);

            if (const auto channel = copied.find (place.track.getRaw()); ! place.onMaster && channel != copied.end())
                place.track = TrackID::fromRaw (channel->second);

            owners.emplace_back (place, getOwnedModulators (song, found->second));
        }
    }

    if (owners.empty())
        return;

    // Each a new ID, in the copied IDs, before any modulation of one is named by them.
    std::vector<ModulatorValue> copies;

    for (const auto& [place, list] : owners)
    {
        for (auto copy : list)
        {
            const auto id = copy.id;
            copy.id = ModulatorID::fromRaw (song.makeID());
            copy.place = place;
            copied[id.getRaw()] = copy.id.getRaw();
            copies.push_back (std::move (copy));
        }
    }

    for (auto& copy : copies)
    {
        auto modulations = immer::flex_vector<ModulationEntry>().transient();

        for (auto modulation : copy.modulations)
        {
            if (const auto found = copied.find (modulation.parameter.device.getRaw()); found != copied.end())
                modulation.parameter.device = DeviceID::fromRaw (found->second);

            modulations.push_back (modulation);
        }

        copy.modulations = modulations.persistent();
        setModulator (song, copy);
    }
}

//==============================================================================
AreaChoice chooseModulationClash (Song& song, const MergeClash& clash, int value, const MergeSides& sides)
{
    // A path through a modulator or a macro ("TRACKS:", "TRACK:12", "MODULATOR:93", "MODULATES:1042/cutoff"): modulators and
    // macros merge as items by their IDs, each property of theirs at a time, what they modulate by its parameter and their
    // values set by hand by their names (SongMerge.cpp); so a clash is a property of one, or one whole (added or taken out on
    // a side), or a property of what is in one (a modulation's amount, a value set by hand), or what is in one whole.
    int step = -1;

    for (int i = 0; i < clash.path.size() && step < 0; ++i)
        if (clash.path[i].startsWith (schema::MODULATOR.toString() + ":") || clash.path[i].startsWith (schema::MACRO.toString() + ":"))
            step = i;

    if (step < 0)
        return AreaChoice::notTheArea;

    // Refused (nothing written) for what a merge of two songs Phazite holds never makes, but a commit's song may come from
    // anyone (tests/FuzzTests.cpp chooses every clash of a damaged merge): a path deeper than what is in one, one whose owner
    // the song hasn't (as the tree's choice found no place for it), one the song holds as the other kind or on another owner,
    // its ID, a property of what is in one that is its key (a modulation's parameter) or that it can't hold (a value of a
    // parameter its kind hasn't), an element of another type or key, one that stands in for what its version couldn't read
    // (SongPieces.h: holdsStandIn()), and one the song would read otherwise than it was chosen (ModulationValue.h:
    // readHeldModulator()).
    if (step == 0 || clash.path.size() > step + 2 || ! juce::isPositiveAndBelow (value, 3))
        return AreaChoice::refused;

    auto ownerPath = clash.path;
    ownerPath.removeRange (step, clash.path.size() - step);
    const auto place = readOwnerPlace (findByPath (song.getState(), ownerPath));
    const auto isMacro = clash.path[step].startsWith (schema::MACRO.toString() + ":");
    const auto id = ModulatorID::fromRaw (readID (clash.path[step].fromFirstOccurrenceOf (":", false, false)));
    const auto held = getModulatorValue (song, id);

    if (! place.has_value() || ! id.isValid() || (held.has_value() && (! isSameOwner (held->place, *place) || held->isMacro() != isMacro)))
        return AreaChoice::refused;

    const juce::ValueTree* elements[] = { &clash.baseElement, &clash.oursElement, &clash.theirsElement };
    const juce::var* values[] = { &clash.base, &clash.ours, &clash.theirs };

    // One's own property: the value chosen given to it (ModulationValue.h: setModulatorProperty(), a void value taking it
    // away as the tree did).
    if (clash.path.size() == step + 1 && clash.property.isValid())
    {
        if (! held.has_value() || clash.property == schema::modulatorID)
            return AreaChoice::refused;

        auto next = *held;

        if (const auto read = setModulatorProperty (next, clash.property, *values[value])
                                  ? readHeldModulator (writeModulatorElement (next), next.place)
                                  : std::nullopt;
            ! read.has_value() || ! (*read == next))
            return AreaChoice::refused;

        setModulator (song, next);
        return AreaChoice::chosen;
    }

    // One whole: the side chosen's (read as the song holds one) made the song's, in its place or after its owner's others,
    // or the song's taken out where that side had none.
    if (clash.path.size() == step + 1)
    {
        const auto& chosen = *elements[value];

        if (! chosen.isValid())
        {
            eraseModulator (song, id);
            return AreaChoice::chosen;
        }

        const auto read =
            getPathStep (chosen) == clash.path[step] && ! holdsStandIn (chosen) ? readHeldModulator (chosen, *place) : std::nullopt;

        if (! read.has_value())
            return AreaChoice::refused;

        setModulator (song, *read);
        return AreaChoice::chosen;
    }

    // What is in one (a MODULATES, a modulator's VALUES): chosen in an element of the one the song holds, as the tree's choice
    // chose it in the song's, and the element read as the song holds one.
    const auto& inPath = clash.path[step + 1];
    const auto isModulates = inPath.startsWith (schema::MODULATES.toString() + ":");

    if (! held.has_value() || ! (isModulates || (inPath.startsWith (schema::VALUES.toString() + ":") && ! held->isMacro())))
        return AreaChoice::refused;

    auto element = writeModulatorElement (*held);
    juce::ValueTree inside;

    for (const auto& child : element)
        if (getPathStep (child) == inPath)
            inside = child;

    if (clash.property.isValid())
    {
        if (isModulates ? clash.property != schema::modulatesAmount
                        : devices::findModulatorParameter (*held->kind, clash.property.toString().toStdString()) < 0)
            return AreaChoice::refused;

        // A modulation the side chosen holds none of, which both lines made (the property's clash has no value before), taken
        // away rather than left without its amount (known bug 239's shape), where that side's song can be read; and one the
        // song holds none of (an earlier choice took it away) put back first as the side chosen holds it.
        const juce::ValueTree* songs[] = { &sides.base, &sides.before, &sides.theirs };
        const auto& side = *songs[value];
        const auto canRead = side.hasType (schema::PHAZITE);
        const auto there = canRead ? findByPath (side, clash.path) : juce::ValueTree();

        if (isModulates && canRead && ! there.isValid())
        {
            if (inside.isValid())
                element.removeChild (inside, nullptr);
        }
        else
        {
            if (! inside.isValid())
            {
                if (isModulates && (! there.isValid() || holdsStandIn (there)))
                    return AreaChoice::refused;

                inside = isModulates ? there.createCopy() : juce::ValueTree (schema::VALUES);
                element.appendChild (inside, nullptr);
            }

            if (values[value]->isVoid())
                inside.removeProperty (clash.property, nullptr);
            else
                inside.setProperty (clash.property, *values[value], nullptr);
        }
    }
    else
    {
        const auto& chosen = *elements[value];

        if (chosen.isValid() && (getPathStep (chosen) != inPath || holdsStandIn (chosen)))
            return AreaChoice::refused;

        const auto at = element.indexOf (inside);

        if (inside.isValid())
            element.removeChild (inside, nullptr);

        if (chosen.isValid())
            element.addChild (chosen.createCopy(), at, nullptr);
    }

    const auto next = readHeldModulator (element, held->place);

    if (! next.has_value())
        return AreaChoice::refused;

    setModulator (song, *next);
    return AreaChoice::chosen;
}
} // namespace phazite
