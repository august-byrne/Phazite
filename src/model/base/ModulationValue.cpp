// SPDX-License-Identifier: AGPL-3.0-or-later
#include "model/base/internal/ModulationValue.h"
#include "model/base/Numbers.h"
#include "model/base/internal/IDsHeld.h"
#include "model/base/internal/SavedValues.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iterator>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    std::atomic<std::uint64_t> modulatorsRead { 0 }; // countModulatorsRead(): a count alone, which orders nothing

    using IDList = immer::flex_vector<ModulatorID>;
    using Lists = immer::map<std::uint64_t, IDList>;

    // A modulator's kind, in ModulatorType's order (model/modulation/ModulationValues.h), the type names' order too.
    static_assert (std::size (schema::preset::modulatorTypeNames) == 6, "a modulator's kinds are the six type names'");

    juce::String toString (std::string_view text)
    {
        return juce::String (text.data(), text.size());
    }

    std::optional<devices::ModulatorKind> findKind (const juce::String& name)
    {
        for (size_t i = 0; i < std::size (schema::preset::modulatorTypeNames); ++i)
            if (name == schema::preset::modulatorTypeNames[i])
                return (devices::ModulatorKind) i;

        return std::nullopt;
    }

    // The list of an owner, by its key: a container's by its ID, a channel's by its track's (the master's under 0).
    bool ownsByContainer (const ModulatorPlace& place)
    {
        return place.container.isValid();
    }

    std::uint64_t getOwnerKey (const ModulatorPlace& place)
    {
        return ownsByContainer (place) ? place.container.getRaw() : place.onMaster ? 0 : place.track.getRaw();
    }

    Lists& getLists (SongModulation& modulation, const ModulatorPlace& place)
    {
        return ownsByContainer (place) ? modulation.containers : modulation.channels;
    }

    bool isSameOwner (const ModulatorPlace& a, const ModulatorPlace& b)
    {
        if (ownsByContainer (a) || ownsByContainer (b))
            return a.container == b.container;

        return a.onMaster == b.onMaster && (a.onMaster || a.track == b.track);
    }

    SongModulation withoutInList (SongModulation modulation, const ModulatorPlace& place, ModulatorID id)
    {
        auto& lists = getLists (modulation, place);
        const auto key = getOwnerKey (place);

        if (const auto* list = lists.find (key))
        {
            for (size_t i = 0; i < list->size(); ++i)
            {
                if ((*list)[i] == id)
                {
                    const auto rest = list->erase (i);
                    lists = rest.empty() ? lists.erase (key) : lists.set (key, rest);
                    break;
                }
            }
        }

        return modulation;
    }

    SongModulation withInList (SongModulation modulation, const ModulatorPlace& place, ModulatorID id, int index)
    {
        auto& lists = getLists (modulation, place);
        const auto key = getOwnerKey (place);
        const auto* found = lists.find (key);
        auto list = found != nullptr ? *found : IDList();
        list = juce::isPositiveAndNotGreaterThan (index, (int) list.size()) ? list.insert ((size_t) index, id) : list.push_back (id);
        lists = lists.set (key, std::move (list));
        return modulation;
    }

    // A number as the song reads one (SongText.h: readValueNumber()), or `otherwise` for none and what is no finite number.
    double readNumber (const juce::var& held, double otherwise)
    {
        const auto number = readValueNumber (held);
        return number.has_value() && std::isfinite (*number) ? *number : otherwise;
    }

    // To a ten-thousandth, as the song holds a step and an amount (withHeldModulator()) and Phazite writes one
    // (writeModulatorElement()).
    double toTenThousandth (double value)
    {
        return std::round (value * 1.0e4) / 1.0e4;
    }

    // A step sequencer's steps as its element writes them, each within -1 to 1, what is no finite number 0.
    std::vector<float> readSteps (const juce::var& written)
    {
        std::vector<float> steps;

        for (const auto& step : juce::StringArray::fromTokens (written.toString(), " ", {}))
            steps.push_back (toFloat (juce::jlimit (-1.0, 1.0, readNumber (step, 0.0))));

        return steps;
    }

    immer::flex_vector<float> toList (const std::vector<float>& steps)
    {
        return immer::flex_vector<float> (steps.begin(), steps.end());
    }

    // A modulator's parameter's value set by hand as the song holds it, from its saved form: a choice by a name it has, or by
    // its number where it is held as a number (not as text), any other by its number, brought within its range and to its
    // steps; none for none, and for what is no finite number.
    std::optional<float> readHeldParameterValue (const devices::ParameterInfo& info, const juce::var& saved)
    {
        if (saved.isVoid())
            return std::nullopt;

        // A choice written as text by one of its names alone, as the loader keeps one (Project.cpp: keepValuesSetByHand()):
        // its number's text ("2") is none.
        if (info.unit == devices::Unit::choice && saved.isString())
        {
            if (const auto choice = devices::findChoice (info, saved.toString().toStdString()))
                return devices::constrain (info, info.minimum + (float) *choice);

            return std::nullopt;
        }

        const auto number = readValueNumber (saved);

        if (! number.has_value() || ! std::isfinite (*number))
            return std::nullopt;

        return devices::constrain (info, toFloat (*number));
    }

    // Each parameter of a kind whose value VALUES hold, by its index, as one of the readers takes it.
    template <typename Read>
    immer::map<std::uint32_t, float> readValues (const juce::ValueTree& modulator, devices::ModulatorKind kind, Read&& read)
    {
        immer::map<std::uint32_t, float> values;
        const auto saved = modulator.getChildWithName (schema::VALUES);

        if (! saved.isValid())
            return values;

        const auto parameters = devices::getModulatorParameters (kind);

        for (size_t i = 0; i < parameters.size(); ++i)
            if (const juce::Identifier name (toString (parameters[i].id)); saved.hasProperty (name))
                if (const auto value = read (parameters[i], saved[name]))
                    values = values.set ((std::uint32_t) i, *value);

        return values;
    }

    // What a modulator or a macro modulates, each MODULATES child read as one of the readers takes it.
    template <typename Read>
    immer::flex_vector<ModulationEntry> readModulations (const juce::ValueTree& element, Read&& read)
    {
        auto modulations = immer::flex_vector<ModulationEntry>().transient();

        for (const auto& child : element)
            if (child.hasType (schema::MODULATES))
                if (const auto entry = read (child))
                    modulations.push_back (*entry);

        return modulations.persistent();
    }

    ParameterRef readModulated (const juce::ValueTree& modulates)
    {
        return { DeviceID::fromRaw (readID (modulates[schema::modulatesDevice])), modulates[schema::modulatesParameter].toString() };
    }

    // A modulation's amount as Phazite's code always read it: its number, 0 for none.
    float readAmount (const juce::ValueTree& modulates)
    {
        return toFloat (readNumber (modulates[schema::modulatesAmount], 0.0));
    }

    // A macro's value as Phazite's code always read it: its number, 0 for none and for what is no finite number.
    float readMacroValue (const juce::ValueTree& macro)
    {
        return toFloat (readNumber (macro[schema::macroValue], 0.0));
    }

    // Each MODULATOR and MACRO element of a song's tree (PHAZITE), in the song's order: each track's (the track list's, in
    // the order of the song's tracks, a folder's tracks after it: SongIndex.h), each channel's own and then its CONTAINERs',
    // each in its element's order; then the master's.
    void forEachModulatorElement (const juce::ValueTree& song, const std::function<void (const juce::ValueTree&)>& each)
    {
        const auto readOwner = [&each] (const juce::ValueTree& owner)
        {
            for (const auto& child : owner)
                if (child.hasType (schema::MODULATOR) || child.hasType (schema::MACRO))
                    each (child);
        };

        const auto readChannel = [&readOwner] (const juce::ValueTree& channel)
        {
            readOwner (channel);

            for (const auto& child : channel)
                if (child.hasType (schema::CONTAINER))
                    readOwner (child);
        };

        const std::function<void (const juce::ValueTree&)> readTracks = [&readChannel, &readTracks] (const juce::ValueTree& parent)
        {
            for (const auto& child : parent)
            {
                if (child.hasType (schema::TRACK))
                {
                    readChannel (child);

                    if (child[schema::trackKind].toString() == schema::trackIsFolder)
                        readTracks (child);
                }
            }
        };

        readTracks (song.getChildWithName (schema::TRACKS));
        readChannel (song.getChildWithName (schema::MASTER));
    }

    // A steps' list as the song holds it: within -1 to 1, to a ten-thousandth, at most so many, without the zeros at the end.
    std::vector<float> keepStepsHeld (std::vector<float> steps)
    {
        if (steps.size() > (size_t) devices::Modulator::maxSteps)
            steps.resize ((size_t) devices::Modulator::maxSteps);

        for (auto& step : steps)
            step = (float) toTenThousandth (juce::jlimit (-1.0, 1.0, (double) step));

        while (! steps.empty() && juce::exactlyEqual (steps.back(), 0.0f))
            steps.pop_back();

        return steps;
    }

    // What both readers read alike: whether the element is one, its ID and its kind.
    std::optional<ModulatorValue> readIdentity (const juce::ValueTree& element, const ModulatorPlace& place)
    {
        ModulatorValue value;

        if (element.hasType (schema::MODULATOR))
        {
            value.kind = readModulatorKind (element);

            if (! value.kind.has_value())
                return std::nullopt;
        }
        else if (! element.hasType (schema::MACRO))
        {
            return std::nullopt;
        }

        value.id = ModulatorID::fromRaw (readID (element[schema::modulatorID]));
        value.place = place;
        modulatorsRead.fetch_add (1, std::memory_order_relaxed);
        return value;
    }

    // Whether an element holds what another does, written as text: the same type, the same properties, each written the
    // same (SongText.h: isSameValue()), and the same children in the same order, each so.
    bool isWrittenAs (const juce::ValueTree& element, const juce::ValueTree& written)
    {
        if (element.getType() != written.getType() || element.getNumProperties() != written.getNumProperties()
            || element.getNumChildren() != written.getNumChildren())
            return false;

        for (int i = 0; i < written.getNumProperties(); ++i)
        {
            const auto name = written.getPropertyName (i);
            const auto* held = element.getPropertyPointer (name);

            if (held == nullptr || ! isSameValue (*held, written[name]))
                return false;
        }

        for (int i = 0; i < written.getNumChildren(); ++i)
            if (! isWrittenAs (element.getChild (i), written.getChild (i)))
                return false;

        return true;
    }
} // namespace

//==============================================================================
bool ModulationEntry::operator== (const ModulationEntry& other) const noexcept
{
    return parameter == other.parameter && juce::exactlyEqual (amount, other.amount);
}

float ModulatorValue::getParameterValue (int index) const
{
    if (! kind.has_value())
        return 0.0f;

    const auto parameters = devices::getModulatorParameters (*kind);

    if (! juce::isPositiveAndBelow (index, (int) parameters.size()))
        return 0.0f;

    const auto* held = values.find ((std::uint32_t) index);
    return held != nullptr ? *held : parameters[(size_t) index].defaultValue;
}

const ModulationEntry* ModulatorValue::findModulation (const ParameterRef& parameter) const
{
    for (const auto& modulation : modulations)
        if (modulation.parameter == parameter)
            return &modulation;

    return nullptr;
}

bool ModulatorValue::isSameAs (const ModulatorValue& other) const noexcept
{
    return id == other.id && place == other.place && kind == other.kind && off == other.off && values.identity() == other.values.identity()
           && steps.identity() == other.steps.identity() && name == other.name && juce::exactlyEqual (value, other.value)
           && modulations.identity() == other.modulations.identity();
}

bool ModulatorValue::operator== (const ModulatorValue& other) const
{
    if (! (id == other.id && place == other.place && kind == other.kind && off == other.off && name == other.name
           && juce::exactlyEqual (value, other.value) && modulations == other.modulations)
        || values.size() != other.values.size() || steps.size() != other.steps.size())
        return false;

    // The values and the steps each compared with exactlyEqual(): immer's own == would compare floats with ==.
    for (const auto& [index, held] : values)
        if (const auto* otherHeld = other.values.find (index); otherHeld == nullptr || ! juce::exactlyEqual (held, *otherHeld))
            return false;

    for (size_t i = 0; i < steps.size(); ++i)
        if (! juce::exactlyEqual (steps[i], other.steps[i]))
            return false;

    return true;
}

const immer::flex_vector<ModulatorID>* SongModulation::findList (const ModulatorPlace& owner) const
{
    return (ownsByContainer (owner) ? containers : channels).find (getOwnerKey (owner));
}

bool SongModulation::isSameAs (const SongModulation& other) const noexcept
{
    return modulators.identity() == other.modulators.identity() && channels.identity() == other.channels.identity()
           && containers.identity() == other.containers.identity();
}

bool SongModulation::operator== (const SongModulation& other) const
{
    return modulators == other.modulators && channels == other.channels && containers == other.containers;
}

SongModulation withModulator (const SongModulation& modulation, ModulatorValue value, int index)
{
    const auto id = value.id;
    const auto place = value.place;
    const auto* old = modulation.find (id);
    auto next = modulation;
    next.modulators = next.modulators.set (id.getRaw(), std::move (value));

    // One whose owner stays keeps its place in the owner's list.
    if (old != nullptr && isSameOwner (old->place, place))
        return next;

    if (old != nullptr)
        next = withoutInList (std::move (next), old->place, id);

    return withInList (std::move (next), place, id, index);
}

SongModulation withoutModulator (const SongModulation& modulation, ModulatorID id)
{
    const auto* old = modulation.find (id);

    if (old == nullptr)
        return modulation;

    auto next = withoutInList (modulation, old->place, id);
    next.modulators = next.modulators.erase (id.getRaw());
    return next;
}

SongModulation withOwnerOrder (const SongModulation& modulation, const ModulatorPlace& owner, immer::flex_vector<ModulatorID> order)
{
    const auto* list = modulation.findList (owner);

    if (list == nullptr || list->size() != order.size())
        return modulation;

    std::set<ModulatorID> held (list->begin(), list->end()), given (order.begin(), order.end());

    if (held != given || given.size() != order.size())
        return modulation;

    auto next = modulation;
    auto& lists = getLists (next, owner);
    lists = lists.set (getOwnerKey (owner), std::move (order));
    return next;
}

bool holdsItsLists (const SongModulation& modulation)
{
    std::set<std::uint64_t> listed;

    for (const auto& [key, value] : modulation.modulators)
        if (key != value.id.getRaw() || (value.place.onMaster && value.place.track.isValid())
            || (! value.place.onMaster && ! value.place.track.isValid()))
            return false;

    const auto holdsLists = [&modulation, &listed] (const Lists& lists, bool ofContainers)
    {
        for (const auto& [key, list] : lists)
        {
            if (list.empty())
                return false;

            for (const auto& id : list)
            {
                const auto* value = modulation.find (id);

                if (value == nullptr || ownsByContainer (value->place) != ofContainers || getOwnerKey (value->place) != key
                    || ! listed.insert (id.getRaw()).second)
                    return false;
            }
        }

        return true;
    };

    return holdsLists (modulation.channels, false) && holdsLists (modulation.containers, true)
           && listed.size() == modulation.modulators.size();
}

SongModulation withHeldModulator (const SongModulation& modulation, ModulatorValue value, int index)
{
    // One modulation of each parameter, the first, each amount within -1 to 1 to a ten-thousandth, as readHeldModulator()
    // reads one: a list that holds them so already kept as it is.
    {
        auto kept = immer::flex_vector<ModulationEntry>().transient();
        std::set<std::pair<std::uint64_t, juce::String>> modulated;
        auto changed = false;

        for (auto entry : value.modulations)
        {
            if (! modulated.insert ({ entry.parameter.device.getRaw(), entry.parameter.parameter }).second)
            {
                changed = true;
                continue;
            }

            if (const auto amount = (float) toTenThousandth (juce::jlimit (-1.0, 1.0, (double) entry.amount));
                ! juce::exactlyEqual (amount, entry.amount))
            {
                entry.amount = amount;
                changed = true;
            }

            kept.push_back (entry);
        }

        if (changed)
            value.modulations = kept.persistent();
    }

    // A modulator's steps as the song holds them (keepStepsHeld()), and none of its values at its parameter's default.
    if (! value.isMacro())
    {
        if (! value.steps.empty())
            if (const auto steps = keepStepsHeld (std::vector<float> (value.steps.begin(), value.steps.end()));
                ! std::equal (steps.begin(), steps.end(), value.steps.begin(), value.steps.end(),
                              [] (float a, float b) { return juce::exactlyEqual (a, b); }))
                value.steps = toList (steps);

        const auto parameters = devices::getModulatorParameters (*value.kind);
        const auto values = value.values;

        for (const auto& [parameter, held] : values)
            if (parameter >= parameters.size() || isDefaultValue (parameters[parameter], held))
                value.values = value.values.erase (parameter);
    }

    // A container's macro on its container's channel: the channel of another of its macros.
    if (ownsByContainer (value.place))
    {
        if (const auto* list = modulation.containers.find (value.place.container.getRaw()))
        {
            for (const auto& other : *list)
            {
                if (const auto* held = modulation.find (other); held != nullptr && other != value.id)
                {
                    value.place.track = held->place.track;
                    value.place.onMaster = held->place.onMaster;
                    break;
                }
            }
        }
    }

    return withModulator (modulation, std::move (value), index);
}

bool holdsTogether (const SongModulation& modulation)
{
    if (! holdsItsLists (modulation))
        return false;

    for (const auto& [key, value] : modulation.modulators)
    {
        std::set<std::pair<std::uint64_t, juce::String>> modulated;

        for (const auto& entry : value.modulations)
            if (! modulated.insert ({ entry.parameter.device.getRaw(), entry.parameter.parameter }).second)
                return false;
    }

    for (const auto& [key, list] : modulation.containers)
    {
        const auto* first = modulation.find (list.front());

        for (const auto& id : list)
            if (const auto* each = modulation.find (id);
                each->place.track != first->place.track || each->place.onMaster != first->place.onMaster)
                return false;
    }

    return true;
}

//==============================================================================
std::optional<devices::ModulatorKind> readModulatorKind (const juce::ValueTree& modulator)
{
    return modulator.hasType (schema::MODULATOR) ? findKind (modulator[schema::modulatorType].toString()) : std::nullopt;
}

ModulatorPlace readModulatorPlace (const juce::ValueTree& element)
{
    const auto channelOf = [] (const juce::ValueTree& channel)
    {
        ModulatorPlace place;

        if (channel.hasType (schema::MASTER))
            place.onMaster = true;
        else if (channel.hasType (schema::TRACK))
            place.track = TrackID::fromRaw (readID (channel[schema::trackID]));

        return place;
    };

    const auto parent = element.getParent();

    if (! parent.hasType (schema::CONTAINER))
        return channelOf (parent);

    auto place = channelOf (parent.getParent());
    place.container = DeviceID::fromRaw (readID (parent[schema::deviceID]));
    return place;
}

std::optional<ModulatorValue> readModulatorValue (const juce::ValueTree& element, const ModulatorPlace& place)
{
    auto value = readIdentity (element, place);

    if (! value.has_value())
        return std::nullopt;

    if (value->isMacro())
    {
        value->name = element[schema::macroName].toString();
        value->value = readMacroValue (element);
    }
    else
    {
        value->off = (bool) element[schema::deviceOff];
        value->values = readValues (element, *value->kind, [] (const devices::ParameterInfo& info, const juce::var& saved)
                                    { return std::optional<float> (readSavedValue (info, saved)); });
        value->steps = toList (readSteps (element[schema::modulatorSteps]));
    }

    value->modulations =
        readModulations (element,
                         [] (const juce::ValueTree& modulates)
                         {
                             return std::optional<ModulationEntry> (
                                 ModulationEntry { .parameter = readModulated (modulates), .amount = readAmount (modulates) });
                         });
    return value;
}

std::optional<ModulatorValue> readHeldModulator (const juce::ValueTree& element, const ModulatorPlace& place)
{
    auto value = readIdentity (element, place);

    if (! value.has_value() || ! value->id.isValid() || value->id.getRaw() >= schema::maxID)
        return std::nullopt;

    if (value->isMacro())
    {
        value->name = element[schema::macroName].toString();
        value->value = (float) readDescribed (element, schema::macroValue);
    }
    else
    {
        value->off = (bool) element[schema::deviceOff];
        value->values = readValues (element, *value->kind,
                                    [] (const devices::ParameterInfo& info, const juce::var& saved) -> std::optional<float>
                                    {
                                        const auto held = readHeldParameterValue (info, saved);
                                        return held.has_value() && ! isDefaultValue (info, *held) ? held : std::nullopt;
                                    });
        value->steps = toList (keepStepsHeld (readSteps (element[schema::modulatorSteps])));
    }

    // One modulation of each parameter, the first; each of a parameter it names.
    std::set<std::pair<std::uint64_t, juce::String>> modulated;
    value->modulations = readModulations (
        element,
        [&modulated] (const juce::ValueTree& modulates) -> std::optional<ModulationEntry>
        {
            const auto parameter = readModulated (modulates);

            if (! parameter.isValid() || ! modulated.insert ({ parameter.device.getRaw(), parameter.parameter }).second)
                return std::nullopt;

            return ModulationEntry { .parameter = parameter,
                                     .amount = (float) toTenThousandth (readDescribed (modulates, schema::modulatesAmount)) };
        });
    return value;
}

juce::ValueTree writeModulatorElement (const ModulatorValue& value)
{
    juce::ValueTree element (value.isMacro() ? schema::MACRO : schema::MODULATOR);
    element.setProperty (schema::modulatorID, (juce::int64) value.id.getRaw(), nullptr);

    if (value.isMacro())
    {
        element.setProperty (schema::macroName, value.name, nullptr);

        if (! juce::exactlyEqual (value.value, 0.0f))
            element.setProperty (schema::macroValue, value.value, nullptr);
    }
    else
    {
        element.setProperty (schema::modulatorType, schema::preset::modulatorTypeNames[(size_t) *value.kind], nullptr);

        if (value.off)
            element.setProperty (schema::deviceOff, true, nullptr);

        const auto parameters = devices::getModulatorParameters (*value.kind);
        juce::ValueTree values (schema::VALUES);

        for (size_t i = 0; i < parameters.size(); ++i)
            if (const auto* held = value.values.find ((std::uint32_t) i))
                values.setProperty (juce::Identifier (toString (parameters[i].id)), toSavedValue (parameters[i], *held), nullptr);

        if (values.getNumProperties() > 0)
            element.appendChild (values, nullptr);

        // Without the zeros at the end, each to a ten-thousandth (as the song holds them: keepStepsHeld()).
        auto count = value.steps.size();

        while (count > 0 && std::abs (value.steps[count - 1]) < 1.0e-4f)
            --count;

        juce::StringArray steps;

        for (size_t i = 0; i < count; ++i)
            steps.add (juce::String (toTenThousandth ((double) value.steps[i])));

        if (! steps.isEmpty())
            element.setProperty (schema::modulatorSteps, steps.joinIntoString (" "), nullptr);
    }

    for (const auto& modulation : value.modulations)
    {
        juce::ValueTree modulates (schema::MODULATES);
        modulates.setProperty (schema::modulatesDevice, juce::String ((juce::int64) modulation.parameter.device.getRaw()), nullptr);
        modulates.setProperty (schema::modulatesParameter, modulation.parameter.parameter, nullptr);
        modulates.setProperty (schema::modulatesAmount, toTenThousandth ((double) modulation.amount), nullptr);
        element.appendChild (modulates, nullptr);
    }

    return element;
}

bool keepModulatorAsHeld (juce::ValueTree element)
{
    const auto held = readHeldModulator (element, readModulatorPlace (element));

    if (! held.has_value())
        return false;

    const auto written = writeModulatorElement (*held);

    if (isWrittenAs (element, written))
        return false;

    element.copyPropertiesAndChildrenFrom (written, nullptr);
    return true;
}

bool isWrittenAsHeld (const juce::ValueTree& element, const ModulatorPlace& place)
{
    const auto held = readHeldModulator (element, place);
    return held.has_value() && isWrittenAs (element, writeModulatorElement (*held));
}

SongModulation readModulationValue (const juce::ValueTree& song)
{
    SongModulation modulation;

    forEachModulatorElement (song,
                             [&modulation] (const juce::ValueTree& element)
                             {
                                 if (auto value = readModulatorValue (element, readModulatorPlace (element));
                                     value.has_value() && modulation.find (value->id) == nullptr)
                                     modulation = withModulator (modulation, std::move (*value));
                             });

    return modulation;
}

std::optional<ModulatorValue> readModulatorValueIn (const juce::ValueTree& song, ModulatorID id)
{
    // The elements' walk of readModulationValue(), only the first of the ID read whole: the others' IDs looked at alone.
    std::optional<ModulatorValue> found;

    forEachModulatorElement (song,
                             [&found, id] (const juce::ValueTree& element)
                             {
                                 if (found.has_value() || readID (element[schema::modulatorID]) != id.getRaw())
                                     return;

                                 if (auto value = readModulatorValue (element, readModulatorPlace (element)); value.has_value())
                                     found = std::move (value);
                             });

    return found;
}

SongModulation readHeldModulation (const juce::ValueTree& song, const std::function<void (const juce::ValueTree&)>& each)
{
    SongModulation modulation;

    forEachModulatorElement (song,
                             [&modulation, &each] (const juce::ValueTree& element)
                             {
                                 if (auto value = readHeldModulator (element, readModulatorPlace (element));
                                     value.has_value() && modulation.find (value->id) == nullptr)
                                 {
                                     if (each)
                                         each (element);

                                     modulation = withHeldModulator (modulation, std::move (*value));
                                 }
                             });

    return modulation;
}

bool setModulatorProperty (ModulatorValue& value, const juce::Identifier& property, const juce::var& held)
{
    if (property == schema::modulatorID)
    {
        value.id = ModulatorID::fromRaw (readID (held));
        return true;
    }

    if (value.isMacro())
    {
        if (property == schema::macroName)
            value.name = held.toString();
        else if (property == schema::macroValue)
            value.value = (float) readDescribed (held, schema::macroValue);
        else
            return false;

        return true;
    }

    if (property == schema::modulatorType)
    {
        const auto kind = findKind (held.toString());

        if (! kind.has_value())
            return false;

        // The values its VALUES would hold, by their parameters' names, read by the new kind's parameters of those names.
        if (*kind != *value.kind)
        {
            const auto before = devices::getModulatorParameters (*value.kind), after = devices::getModulatorParameters (*kind);
            immer::map<std::uint32_t, float> values;

            for (const auto& [index, was] : value.values)
            {
                if (index >= before.size())
                    continue;

                if (const auto now = devices::findModulatorParameter (*kind, before[index].id); now >= 0)
                    if (const auto read = readHeldParameterValue (after[(size_t) now], toSavedValue (before[index], was));
                        read.has_value() && ! isDefaultValue (after[(size_t) now], *read))
                        values = values.set ((std::uint32_t) now, *read);
            }

            value.values = values;
            value.kind = kind;
        }

        return true;
    }

    if (property == schema::deviceOff)
        value.off = (bool) held;
    else if (property == schema::modulatorSteps)
        value.steps = toList (keepStepsHeld (readSteps (held)));
    else
        return false;

    return true;
}

std::uint64_t findLargestIDIn (const SongModulation& modulation)
{
    std::uint64_t largest = 0;

    for (const auto& [id, value] : modulation.modulators)
        largest = countHeldID (largest, id);

    return largest;
}

void forEachIDHeld (const SongModulation& modulation, const std::function<void (std::uint64_t)>& each)
{
    for (const auto& [id, value] : modulation.modulators)
        each (id);
}

void forEachIDNamed (const SongModulation& modulation, const std::function<void (std::uint64_t)>& each)
{
    for (const auto& [id, value] : modulation.modulators)
        for (const auto& entry : value.modulations)
            each (entry.parameter.device.getRaw());
}

std::uint64_t countModulatorsRead() noexcept
{
    return modulatorsRead.load (std::memory_order_relaxed);
}
} // namespace phazite
