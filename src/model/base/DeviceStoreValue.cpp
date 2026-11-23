// SPDX-License-Identifier: AGPL-3.0-or-later
#include "model/base/internal/DeviceStoreValue.h"
#include "model/base/Numbers.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <utility>

namespace phazite
{
std::size_t PluginValueKeyHash::operator() (const PluginValueKey& key) const noexcept
{
    return std::hash<std::uint64_t>() (key.device.getRaw()) * 31u + (std::size_t) key.parameter.hashCode64();
}

bool PluginValueEntry::operator== (const PluginValueEntry& other) const noexcept
{
    return juce::exactlyEqual (value, other.value) && isSongs == other.isSongs;
}

const juce::MemoryBlock& PluginBytes::get() const noexcept
{
    static const juce::MemoryBlock none;
    return bytes != nullptr ? *bytes : none;
}

bool PluginBytes::operator== (const PluginBytes& other) const noexcept
{
    return bytes == other.bytes || get() == other.get();
}

bool DeviceStore::isSameAs (const DeviceStore& other) const noexcept
{
    return pluginValues.identity() == other.pluginValues.identity() && states.identity() == other.states.identity()
           && programs.identity() == other.programs.identity() && formatIDs.identity() == other.formatIDs.identity()
           && versions.identity() == other.versions.identity() && chainMutes.identity() == other.chainMutes.identity()
           && chainSolos.identity() == other.chainSolos.identity();
}

namespace
{
    // Whether each of a store's plug-in values that is the song's is the other's too (the held ones, which aren't, aside).
    bool holdsSongsValuesOf (const DeviceStore& store, const DeviceStore& other)
    {
        for (const auto& [key, entry] : store.pluginValues)
        {
            if (! entry.isSongs)
                continue;

            const auto* there = other.pluginValues.find (key);

            if (there == nullptr || ! (*there == entry))
                return false;
        }

        return true;
    }
} // namespace

bool DeviceStore::operator== (const DeviceStore& other) const
{
    if (isSameAs (other))
        return true;

    return states == other.states && programs == other.programs && formatIDs == other.formatIDs && versions == other.versions
           && chainMutes == other.chainMutes && chainSolos == other.chainSolos && holdsSongsValuesOf (*this, other)
           && holdsSongsValuesOf (other, *this);
}

DeviceStoreChanges findChanges (const DeviceStore& before, const DeviceStore& after)
{
    DeviceStoreChanges changes;
    changes.pluginValues = findMapChanges (before.pluginValues, after.pluginValues);
    changes.states = findMapChanges (before.states, after.states);
    changes.programs = findMapChanges (before.programs, after.programs);
    changes.formatIDs = findMapChanges (before.formatIDs, after.formatIDs);
    changes.versions = findMapChanges (before.versions, after.versions);
    changes.chainMutes = findMapChanges (before.chainMutes, after.chainMutes);
    changes.chainSolos = findMapChanges (before.chainSolos, after.chainSolos);
    return changes;
}

DeviceStore withChanges (DeviceStore now, const DeviceStoreChanges& changes, ChangeDirection direction)
{
    now.pluginValues = withMapChanges (std::move (now.pluginValues), changes.pluginValues, direction);
    now.states = withMapChanges (std::move (now.states), changes.states, direction);
    now.programs = withMapChanges (std::move (now.programs), changes.programs, direction);
    now.formatIDs = withMapChanges (std::move (now.formatIDs), changes.formatIDs, direction);
    now.versions = withMapChanges (std::move (now.versions), changes.versions, direction);
    now.chainMutes = withMapChanges (std::move (now.chainMutes), changes.chainMutes, direction);
    now.chainSolos = withMapChanges (std::move (now.chainSolos), changes.chainSolos, direction);
    return now;
}

PluginValueEntry makeHeldPluginValue (float value)
{
    PluginValueEntry entry;
    entry.value = value;
    entry.isSongs = false;
    return entry;
}

PluginValueEntry makeSongsPluginValue (float value)
{
    PluginValueEntry entry;
    entry.value = value;
    return entry;
}

//==============================================================================
namespace
{
    // How a number of the store's is read: as Phazite's code always read it, or as the song holds it.
    enum class Reading
    {
        lenient,
        asHeld
    };

    // A plug-in's state's bytes from base 64, none for none, for text that isn't base 64 and for no bytes.
    std::optional<juce::MemoryBlock> readStateBytes (const juce::var& held)
    {
        if (const auto text = held.toString(); text.isNotEmpty())
            if (auto bytes = readBytes (text); bytes.has_value() && bytes->getSize() > 0)
                return std::move (*bytes);

        return std::nullopt;
    }

    // A program, none for none, as an int reads it, both ways (Schema.h gives it no range; one below 0, which a damaged file
    // may hold, is kept as it is, and what reads it takes it as 0: model/plugins/PluginInstances.cpp).
    std::optional<int> readProgram (const juce::var& held)
    {
        if (held.isVoid())
            return std::nullopt;

        return toWhole<int> (held);
    }

    // A VALUE's value as a float reads it, or as the song holds it, by its description.
    float readPluginValue (const juce::ValueTree& value, Reading reading)
    {
        if (reading == Reading::lenient)
            return (float) value[schema::valueSetByHand];

        return (float) readDescribed (value, schema::valueSetByHand);
    }

    void readStore (const juce::ValueTree& element, DeviceStore& store, Reading reading)
    {
        const auto id = readDeviceElementID (element);

        if (! id.isValid())
            return;

        const auto raw = id.getRaw();

        if (element.hasType (schema::CHAIN))
        {
            if ((bool) element[schema::chainMute])
                store.chainMutes = store.chainMutes.set (raw, true);

            if ((bool) element[schema::chainSolo])
                store.chainSolos = store.chainSolos.set (raw, true);

            return;
        }

        if (readDeviceForm (element) != DeviceValue::Form::plugin)
            return;

        if (auto bytes = readStateBytes (element[schema::pluginState]))
            store.states = store.states.set (raw, PluginBytes { .bytes = std::make_shared<const juce::MemoryBlock> (std::move (*bytes)) });

        if (const auto program = readProgram (element[schema::pluginProgram]))
            store.programs = store.programs.set (raw, *program);

        if (const auto formatID = element[schema::pluginFormatID].toString(); formatID.isNotEmpty())
            store.formatIDs = store.formatIDs.set (raw, formatID);

        if (const auto version = element[schema::pluginVersion].toString(); version.isNotEmpty())
            store.versions = store.versions.set (raw, version);

        for (const auto& child : element)
        {
            if (! child.hasType (schema::VALUE))
                continue;

            PluginValueKey key;
            key.device = id;
            key.parameter = child[schema::valueParameter].toString();

            if (store.pluginValues.find (key) == nullptr) // the first of one parameter, as the tree's readers find it
                store.pluginValues = store.pluginValues.set (key, makeSongsPluginValue (readPluginValue (child, reading)));
        }
    }
} // namespace

void readStoreOf (const juce::ValueTree& element, DeviceStore& store)
{
    readStore (element, store, Reading::lenient);
}

void readHeldStoreOf (const juce::ValueTree& element, DeviceStore& store)
{
    readStore (element, store, Reading::asHeld);
}

bool isPluginParameterBefore (const juce::String& a, const juce::String& b)
{
    if (const auto natural = a.compareNatural (b); natural != 0)
        return natural < 0;

    return a.compare (b) < 0;
}

void writeStoreInto (juce::ValueTree& element, const DeviceStore& store, DeviceID id)
{
    const auto raw = id.getRaw();

    if (element.hasType (schema::CHAIN))
    {
        if (const auto* muted = store.chainMutes.find (raw); muted != nullptr && *muted)
            element.setProperty (schema::chainMute, true, nullptr);

        if (const auto* soloed = store.chainSolos.find (raw); soloed != nullptr && *soloed)
            element.setProperty (schema::chainSolo, true, nullptr);

        return;
    }

    if (! element.hasType (schema::DEVICE))
        return;

    if (const auto* state = store.states.find (raw); state != nullptr && state->get().getSize() > 0)
        element.setProperty (schema::pluginState, writeBytes (state->get()), nullptr);

    if (const auto* formatID = store.formatIDs.find (raw); formatID != nullptr && formatID->isNotEmpty())
        element.setProperty (schema::pluginFormatID, *formatID, nullptr);

    if (const auto* version = store.versions.find (raw); version != nullptr && version->isNotEmpty())
        element.setProperty (schema::pluginVersion, *version, nullptr);

    if (const auto* program = store.programs.find (raw))
        element.setProperty (schema::pluginProgram, *program, nullptr);

    // The song's values of this device, sorted by their parameters: a map has no order, and VALUE no order group.
    std::vector<std::pair<juce::String, float>> values;

    for (const auto& [key, entry] : store.pluginValues)
        if (key.device == id && entry.isSongs)
            values.emplace_back (key.parameter, entry.value);

    std::sort (values.begin(), values.end(), [] (const auto& a, const auto& b) { return isPluginParameterBefore (a.first, b.first); });

    for (const auto& [parameter, value] : values)
    {
        juce::ValueTree written (schema::VALUE, { { schema::valueParameter, parameter } });
        written.setProperty (schema::valueSetByHand, value, nullptr);
        element.appendChild (written, nullptr);
    }
}
} // namespace phazite
