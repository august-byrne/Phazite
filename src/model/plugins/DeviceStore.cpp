// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/DeviceStore.h"
#include "model/base/Numbers.h"
#include "model/base/Song.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    // Sets a property as ValueTree::setProperty() does with an undo manager, which leaves a value that is the same as JUCE
    // compares values alone (a number read from a file is text), but without one. JUCE workaround W8
    // (docs/juce-workarounds.md).
    void storeProperty (juce::ValueTree& element, const juce::Identifier& property, const juce::var& value)
    {
        if (const auto* stored = element.getPropertyPointer (property); stored == nullptr || *stored != value)
            element.setProperty (property, value, nullptr);
    }

    // A plug-in's element in the song by its ID, invalid for a device the song hasn't or one that isn't a plug-in.
    juce::ValueTree findPlugin (const Song& song, DeviceID id)
    {
        const auto device = findDeviceState (song, id);
        return isPluginState (device) ? device : juce::ValueTree();
    }

    // A plug-in's VALUE of a parameter, the first, invalid for none.
    juce::ValueTree findValueOf (const juce::ValueTree& device, const juce::String& parameter)
    {
        return device.getChildWithProperty (schema::valueParameter, parameter);
    }

    juce::MemoryBlock readState (const juce::ValueTree& device)
    {
        if (const auto text = device[schema::pluginState].toString(); text.isNotEmpty())
            if (auto bytes = readBytes (text))
                return std::move (*bytes);

        return {};
    }

    const juce::Identifier& getProperty (ChainFlag flag)
    {
        switch (flag)
        {
            case ChainFlag::mute:
                return schema::chainMute.getName();
            case ChainFlag::solo:
                break;
        }

        return schema::chainSolo.getName();
    }
} // namespace

//==============================================================================
juce::MemoryBlock getPluginState (const Song& song, DeviceID id)
{
    return getPluginState (findPlugin (song, id));
}

std::optional<int> getPluginProgram (const Song& song, DeviceID id)
{
    return getPluginProgram (findPlugin (song, id));
}

juce::MemoryBlock getPluginState (const juce::ValueTree& device)
{
    return readState (device);
}

std::optional<int> getPluginProgram (const juce::ValueTree& device)
{
    if (const auto* program = device.getPropertyPointer (schema::pluginProgram))
        return toWhole<int> (*program);

    return std::nullopt;
}

juce::String getPluginFormatID (const Song& song, DeviceID id)
{
    return findPlugin (song, id)[schema::pluginFormatID].toString();
}

juce::String getPluginVersion (const Song& song, DeviceID id)
{
    return findPlugin (song, id)[schema::pluginVersion].toString();
}

std::vector<PluginValueSet> getPluginValues (const Song& song, DeviceID id)
{
    return getPluginValues (findPlugin (song, id));
}

std::optional<float> findPluginValue (const Song& song, DeviceID id, const juce::String& parameter)
{
    return findPluginValue (findPlugin (song, id), parameter);
}

std::vector<PluginValueSet> getPluginValues (const juce::ValueTree& device)
{
    std::vector<PluginValueSet> values;

    for (const auto& child : device)
        if (child.hasType (schema::VALUE))
            values.push_back (readPluginValue (child));

    return values;
}

std::optional<float> findPluginValue (const juce::ValueTree& device, const juce::String& parameter)
{
    if (const auto value = findValueOf (device, parameter); value.isValid())
        return (float) value[schema::valueSetByHand];

    return std::nullopt;
}

PluginValueSet readPluginValue (const juce::ValueTree& value)
{
    return { .parameter = value[schema::valueParameter].toString(), .value = (float) value[schema::valueSetByHand] };
}

//==============================================================================
void storePluginIntoHeld (juce::ValueTree device, const StoredPlugin& stored)
{
    if (! device.isValid())
        return;

    if (stored.state != nullptr)
    {
        if (stored.state->getSize() > 0)
            storeProperty (device, schema::pluginState, writeBytes (*stored.state));
        else
            device.removeProperty (schema::pluginState, nullptr);
    }

    if (stored.formatID.isNotEmpty())
        storeProperty (device, schema::pluginFormatID, stored.formatID);

    if (stored.version.isNotEmpty())
        storeProperty (device, schema::pluginVersion, stored.version);

    if (stored.program.has_value())
    {
        if (*stored.program >= 0)
            storeProperty (device, schema::pluginProgram, *stored.program);
        else
            device.removeProperty (schema::pluginProgram, nullptr);
    }

    for (const auto& [parameter, value] : stored.values)
    {
        auto held = findValueOf (device, parameter);

        if (! held.isValid())
        {
            held = juce::ValueTree (schema::VALUE, { { schema::valueParameter, parameter } });
            device.appendChild (held, nullptr);
        }

        storeProperty (held, schema::valueSetByHand, value);
    }
}

void storePluginOutsideHistory (Song& song, DeviceID id, const StoredPlugin& stored)
{
    storePluginOutsideHistory (findPlugin (song, id), stored);
}

void storePluginOutsideHistory (juce::ValueTree device, const StoredPlugin& stored)
{
    storePluginIntoHeld (std::move (device), stored);
}

void setPluginValueOutsideHistory (Song& song, DeviceID id, const juce::String& parameter, float value)
{
    if (auto device = findDeviceState (song, id); device.isValid())
        for (auto child : device)
            if (child.hasType (schema::VALUE) && child[schema::valueParameter].toString() == parameter)
                storeProperty (child, schema::valueSetByHand, value);
}

void setPluginStateInStep (Song& song, DeviceID id, const juce::String& base64)
{
    setPluginStateInStep (song, findPlugin (song, id), base64);
}

void setPluginStateInStep (Song& song, DeviceID id, const juce::MemoryBlock& state)
{
    setPluginStateInStep (song, id, writeBytes (state));
}

void setPluginProgramInStep (Song& song, DeviceID id, int program)
{
    setPluginProgramInStep (song, findPlugin (song, id), program);
}

void setPluginStateInStep (Song& song, juce::ValueTree device, const juce::String& base64)
{
    if (! device.isValid())
        return;

    if (base64.isNotEmpty())
        device.setProperty (schema::pluginState, base64, &song.getUndoManager());
    else
        device.removeProperty (schema::pluginState, &song.getUndoManager());
}

void setPluginProgramInStep (Song& song, juce::ValueTree device, int program)
{
    if (device.isValid())
        device.setProperty (schema::pluginProgram, program, &song.getUndoManager());
}

void setPluginValueSetByHand (Song& song, DeviceID id, const juce::String& parameter, float value, float held)
{
    setPluginValueSetByHand (song, findPlugin (song, id), parameter, value, held);
}

void removePluginValueSetByHand (Song& song, DeviceID id, const juce::String& parameter)
{
    removePluginValueSetByHand (song, findPlugin (song, id), parameter);
}

void setPluginValueSetByHand (Song& song, juce::ValueTree device, const juce::String& parameter, float value, float held)
{
    if (! device.isValid())
        return;

    auto* undo = &song.getUndoManager();
    auto stored = findValueOf (device, parameter);

    if (! stored.isValid())
    {
        stored = juce::ValueTree (schema::VALUE, { { schema::valueParameter, parameter }, { schema::valueSetByHand, held } });
        device.appendChild (stored, undo);
    }

    stored.setProperty (schema::valueSetByHand, value, undo);
}

void removePluginValueSetByHand (Song& song, juce::ValueTree device, const juce::String& parameter)
{
    if (device.isValid())
        if (const auto stored = findValueOf (device, parameter); stored.isValid())
            device.removeChild (stored, &song.getUndoManager());
}

//==============================================================================
bool isChainMuted (const Song& song, DeviceID chain)
{
    return isChainMuted (findContainerChain (song, chain));
}

bool isChainSoloed (const Song& song, DeviceID chain)
{
    return isChainSoloed (findContainerChain (song, chain));
}

bool isChainMuted (const juce::ValueTree& chain)
{
    return chain[schema::chainMute];
}

bool isChainSoloed (const juce::ValueTree& chain)
{
    return chain[schema::chainSolo];
}

void setChainSwitch (Song& song, DeviceID chain, ChainFlag flag, bool on)
{
    auto state = findContainerChain (song, chain);
    const auto& property = getProperty (flag);

    if (! state.isValid() || (bool) state[property] == on)
        return;

    if (on)
        state.setProperty (property, true, nullptr);
    else
        state.removeProperty (property, nullptr);
}

void clearChainSwitchesInStep (Song& song, DeviceID chain)
{
    clearChainSwitchesInStep (song, findContainerChain (song, chain));
}

void clearChainSwitchesInStep (Song& song, juce::ValueTree chain)
{
    if (chain.isValid())
    {
        chain.removeProperty (schema::chainMute, &song.getUndoManager());
        chain.removeProperty (schema::chainSolo, &song.getUndoManager());
    }
}

//==============================================================================
ChannelStore getChannelStore (const Song& song, TrackID channel)
{
    ChannelStore held;

    // An item's entries and its window's place, then what it holds: a container's chains, a chain's devices.
    const std::function<void (const juce::ValueTree&)> readItem = [&held, &readItem] (const juce::ValueTree& item)
    {
        readStoreOf (item, held.store);

        if (item.hasProperty (schema::deviceWindowX))
            held.windows.emplace_back (readDeviceElementID (item), WindowPlace { .x = toWhole<int> (item[schema::deviceWindowX]),
                                                                                 .y = toWhole<int> (item[schema::deviceWindowY]) });

        for (const auto& child : item)
            if (child.hasType (schema::CHAIN) || child.hasType (schema::DEVICE))
                readItem (child);
    };

    for (const auto& child : findChainState (song, channel))
        if (child.hasType (schema::DEVICE) || child.hasType (schema::CONTAINER))
            readItem (child);

    return held;
}

DeviceStore renameStore (const DeviceStore& from, const std::map<std::uint64_t, std::uint64_t>& copied)
{
    DeviceStore renamed;

    // Each map's entries of an item copied, under its copy's ID.
    const auto rename = [&copied] (const auto& entries, auto& into)
    {
        for (const auto& [key, entry] : entries)
            if (const auto found = copied.find (key); found != copied.end())
                into = into.set (found->second, entry);
    };

    rename (from.states, renamed.states);
    rename (from.programs, renamed.programs);
    rename (from.formatIDs, renamed.formatIDs);
    rename (from.versions, renamed.versions);
    rename (from.chainMutes, renamed.chainMutes);
    rename (from.chainSolos, renamed.chainSolos);

    for (const auto& [key, entry] : from.pluginValues)
    {
        if (const auto found = copied.find (key.device.getRaw()); found != copied.end())
        {
            auto copy = key;
            copy.device = DeviceID::fromRaw (found->second);
            renamed.pluginValues = renamed.pluginValues.set (copy, entry);
        }
    }

    return renamed;
}

void writeStoreIntoMade (juce::ValueTree& made, const DeviceStore& store, DeviceID id)
{
    jassert (! made.getParent().isValid()); // an element not in a song yet
    writeStoreInto (made, store, id);
}

//==============================================================================
namespace
{
    // A device's, a container's or a chain's element by its ID (a walk, and another for a chain), invalid for none.
    juce::ValueTree findItem (const Song& song, DeviceID id)
    {
        if (auto device = findDeviceState (song, id); device.isValid())
            return device;

        return findContainerChain (song, id);
    }

    // The song's tree written as the song is written now: in the step in progress, or outside the history.
    juce::UndoManager* getUndoNow (Song& song)
    {
        return song.isWritingOutsideHistory() ? nullptr : &song.getUndoManager();
    }

    // Whether two of a store's maps hold the same of one key: both none, or both alike.
    template <typename Map>
    bool holdSameEntry (const Map& a, const Map& b, std::uint64_t key)
    {
        const auto* x = a.find (key);
        const auto* y = b.find (key);
        return (x == nullptr) == (y == nullptr) && (x == nullptr || *x == *y);
    }

    // A chain's switch made what the entries hold of it.
    bool writeChainFlag (juce::ValueTree& chain, const juce::Identifier& property, bool on, juce::UndoManager* undo)
    {
        if ((bool) chain[property] == on && (on || ! chain.hasProperty (property)))
            return false;

        if (on)
            chain.setProperty (property, true, undo);
        else
            chain.removeProperty (property, undo);

        return true;
    }

    std::atomic<std::uint64_t> placesBuilt { 0 }; // countPluginValuePlacesBuilt(): a count alone, which orders nothing

    // A plug-in's VALUE children made the song's values the entries hold of it, then put in the order given: each found by its
    // parameter in an index made once (the first of two of one parameter, as findValueOf() finds it), and the places the
    // VALUE children stand at listed once (countPluginValuePlacesBuilt()), each moved to its place there; again after a move
    // only where something else stands among them, which a move shifts. A cost that grows with the values' count, not with
    // its square (tests/DevicesVersionsTests.cpp).
    bool writePluginValues (juce::ValueTree& device, DeviceID id, const DeviceStore& entries, const std::vector<juce::String>& order,
                            juce::UndoManager* undo)
    {
        auto changed = false;
        const auto findEntry = [&entries, id] (const juce::String& parameter)
        {
            PluginValueKey key;
            key.device = id;
            key.parameter = parameter;
            const auto* entry = entries.pluginValues.find (key);
            return entry != nullptr && entry->isSongs ? entry : nullptr;
        };

        // Those the entries hold none of taken away.
        for (int i = device.getNumChildren(); --i >= 0;)
        {
            if (const auto child = device.getChild (i);
                child.hasType (schema::VALUE) && findEntry (child[schema::valueParameter].toString()) == nullptr)
            {
                device.removeChild (i, undo);
                changed = true;
            }
        }

        // The VALUE children left, by their parameters (the first of each).
        std::map<juce::String, juce::ValueTree> held;

        for (const auto& child : device)
            if (child.hasType (schema::VALUE))
                held.emplace (child[schema::valueParameter].toString(), child);

        // Each the entries hold set where it differs (as the song holds it: DeviceStoreValue.h, readHeldStoreOf()), or made.
        for (const auto& [key, entry] : entries.pluginValues)
        {
            if (key.device != id || ! entry.isSongs)
                continue;

            if (const auto found = held.find (key.parameter); found != held.end())
            {
                if (auto value = found->second; ! juce::exactlyEqual ((float) readDescribed (value, schema::valueSetByHand), entry.value))
                {
                    value.setProperty (schema::valueSetByHand, entry.value, undo);
                    changed = true;
                }
            }
            else
            {
                juce::ValueTree made (schema::VALUE, { { schema::valueParameter, key.parameter } });
                made.setProperty (schema::valueSetByHand, entry.value, nullptr);
                device.appendChild (made, undo);
                held.emplace (key.parameter, made);
                changed = true;
            }
        }

        // Their order: those the order names, in it, then the others in theirs.
        std::vector<juce::ValueTree> wanted;
        std::set<juce::String> named;

        for (const auto& parameter : order)
            if (const auto found = held.find (parameter); found != held.end() && named.insert (parameter).second)
                wanted.push_back (found->second);

        for (const auto& child : device)
            if (child.hasType (schema::VALUE))
                if (const auto parameter = child[schema::valueParameter].toString(); named.insert (parameter).second)
                    wanted.push_back (held[parameter]);

        // The places the VALUE children stand at, in their order: the same places after a move among them while nothing else
        // stands among them (a plug-in's element holds its VALUE children alone), else listed again after each move.
        const auto listPlaces = [&device]
        {
            placesBuilt.fetch_add (1, std::memory_order_relaxed);
            std::vector<int> places;

            for (int i = 0; i < device.getNumChildren(); ++i)
                if (device.getChild (i).hasType (schema::VALUE))
                    places.push_back (i);

            return places;
        };

        auto places = listPlaces();
        const auto together = places.empty() || places.back() - places.front() + 1 == (int) places.size();

        for (size_t k = 0; k < wanted.size() && k < places.size(); ++k)
        {
            if (device.getChild (places[k]) == wanted[k])
                continue;

            device.moveChild (device.indexOf (wanted[k]), places[k], undo);
            changed = true;

            if (! together)
                places = listPlaces();
        }

        return changed;
    }
} // namespace

DeviceStore getStoreOf (const Song& song, DeviceID id)
{
    DeviceStore store;

    if (const auto item = findItem (song, id); item.isValid())
        readHeldStoreOf (item, store);

    return store;
}

std::vector<juce::String> getPluginValueOrder (const Song& song, DeviceID id)
{
    std::vector<juce::String> order;

    for (const auto& [parameter, value] : getPluginValues (song, id))
        order.push_back (parameter);

    return order;
}

bool setStoreEntries (Song& song, DeviceID id, const DeviceStore& entries, const std::vector<juce::String>& valueOrder)
{
    auto item = findItem (song, id);
    auto* undo = getUndoNow (song);
    DeviceStore held;
    readHeldStoreOf (item, held);
    const auto raw = id.getRaw();

    if (item.hasType (schema::CHAIN))
    {
        const auto isOn = [raw] (const immer::map<std::uint64_t, bool>& flags)
        {
            const auto* on = flags.find (raw);
            return on != nullptr && *on;
        };

        const auto muted = writeChainFlag (item, schema::chainMute, isOn (entries.chainMutes), undo);
        const auto soloed = writeChainFlag (item, schema::chainSolo, isOn (entries.chainSolos), undo);
        return muted || soloed;
    }

    if (! isPluginState (item))
        return false;

    auto changed = false;

    if (! holdSameEntry (held.states, entries.states, raw))
    {
        if (const auto* state = entries.states.find (raw); state != nullptr && state->get().getSize() > 0)
            item.setProperty (schema::pluginState, writeBytes (state->get()), undo);
        else
            item.removeProperty (schema::pluginState, undo);

        changed = true;
    }

    if (! holdSameEntry (held.programs, entries.programs, raw))
    {
        if (const auto* program = entries.programs.find (raw))
            item.setProperty (schema::pluginProgram, *program, undo);
        else
            item.removeProperty (schema::pluginProgram, undo);

        changed = true;
    }

    for (const auto& [property, map] :
         { std::pair (&schema::pluginFormatID, &DeviceStore::formatIDs), std::pair (&schema::pluginVersion, &DeviceStore::versions) })
    {
        if (holdSameEntry (held.*map, entries.*map, raw))
            continue;

        if (const auto* text = (entries.*map).find (raw); text != nullptr && text->isNotEmpty())
            item.setProperty (*property, *text, undo);
        else
            item.removeProperty (*property, undo);

        changed = true;
    }

    return writePluginValues (item, id, entries, valueOrder, undo) || changed;
}

std::uint64_t countPluginValuePlacesBuilt() noexcept
{
    return placesBuilt.load (std::memory_order_relaxed);
}

std::optional<WindowPlace> readWindowPlace (const juce::ValueTree& device)
{
    if (device.hasProperty (schema::deviceWindowX))
        return WindowPlace { .x = toWhole<int> (device[schema::deviceWindowX]), .y = toWhole<int> (device[schema::deviceWindowY]) };

    return std::nullopt;
}

void forgetWindowPlace (Song& song, DeviceID id)
{
    if (auto state = findDeviceState (song, id); state.isValid())
    {
        state.removeProperty (schema::deviceWindowX, nullptr);
        state.removeProperty (schema::deviceWindowY, nullptr);
    }
}

//==============================================================================
std::optional<WindowPlace> getWindowPlace (const Song& song, DeviceID id)
{
    return readWindowPlace (findDeviceState (song, id));
}

void setWindowPlace (Song& song, DeviceID id, WindowPlace place)
{
    if (auto state = findDeviceState (song, id); state.isValid())
    {
        storeProperty (state, schema::deviceWindowX, place.x);
        storeProperty (state, schema::deviceWindowY, place.y);
    }
}
} // namespace phazite
