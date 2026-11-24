// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/DeviceStore.h"
#include "model/base/Song.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/SongValues.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    std::atomic<std::uint64_t> placesBuilt { 0 }; // countPluginValuePlacesBuilt(): a count alone, which orders nothing

    const DeviceStore& getStore (const Song& song)
    {
        static const DeviceStore none;
        const auto* values = song.getValues();
        return values != nullptr ? values->deviceStore.get() : none;
    }

    // An item the song holds by its ID, as the devices' area holds it (a look in a map: no walk), nullptr for none.
    const DeviceValue* findHeld (const Song& song, DeviceID id)
    {
        const auto* values = song.getValues();
        return values != nullptr && id.isValid() ? values->devices.get().find (id) : nullptr;
    }

    bool holdsForm (const Song& song, DeviceID id, DeviceValue::Form form)
    {
        const auto* held = findHeld (song, id);
        return held != nullptr && held->form == form;
    }

    bool holdsPlugin (const Song& song, DeviceID id)
    {
        return holdsForm (song, id, DeviceValue::Form::plugin);
    }

    bool holdsChain (const Song& song, DeviceID id)
    {
        return holdsForm (song, id, DeviceValue::Form::chain);
    }

    PluginValueKey makeKey (DeviceID device, const juce::String& parameter)
    {
        PluginValueKey key;
        key.device = device;
        key.parameter = parameter;
        return key;
    }

    PluginBytes makeBytes (const juce::MemoryBlock& block)
    {
        return { .bytes = std::make_shared<const juce::MemoryBlock> (block) };
    }

    // A map's entry of an item set to a value, the very map where it holds it so.
    template <typename Map, typename Entry>
    Map withEntry (const Map& map, std::uint64_t key, const Entry& entry)
    {
        const auto* held = map.find (key);
        return held != nullptr && *held == entry ? map : map.set (key, entry);
    }

    template <typename Map>
    Map withoutEntry (const Map& map, std::uint64_t key)
    {
        return map.find (key) != nullptr ? map.erase (key) : map;
    }

    // A plug-in's song's value of a parameter set, the very store where it holds it so.
    DeviceStore withSongsValue (const DeviceStore& store, DeviceID id, const juce::String& parameter, float value)
    {
        const auto key = makeKey (id, parameter);
        const auto entry = makeSongsPluginValue (value);

        if (const auto* held = store.pluginValues.find (key); held != nullptr && *held == entry)
            return store;

        auto next = store;
        next.pluginValues = next.pluginValues.set (key, entry);
        return next;
    }

    SongValues* findValues (Song& song)
    {
        return song.getValues();
    }
} // namespace

//==============================================================================
juce::MemoryBlock getPluginState (const Song& song, DeviceID id)
{
    return getPluginStateBytes (song, id).get();
}

PluginBytes getPluginStateBytes (const Song& song, DeviceID id)
{
    if (! holdsPlugin (song, id))
        return {};

    const auto* state = getStore (song).states.find (id.getRaw());
    return state != nullptr ? *state : PluginBytes();
}

std::optional<int> getPluginProgram (const Song& song, DeviceID id)
{
    if (! holdsPlugin (song, id))
        return std::nullopt;

    const auto* program = getStore (song).programs.find (id.getRaw());
    return program != nullptr ? std::optional (*program) : std::nullopt;
}

juce::String getPluginFormatID (const Song& song, DeviceID id)
{
    const auto* text = holdsPlugin (song, id) ? getStore (song).formatIDs.find (id.getRaw()) : nullptr;
    return text != nullptr ? *text : juce::String();
}

juce::String getPluginVersion (const Song& song, DeviceID id)
{
    const auto* text = holdsPlugin (song, id) ? getStore (song).versions.find (id.getRaw()) : nullptr;
    return text != nullptr ? *text : juce::String();
}

std::vector<PluginValueSet> getPluginValues (const Song& song, DeviceID id)
{
    std::vector<PluginValueSet> values;

    if (! holdsPlugin (song, id))
        return values;

    for (const auto& [key, entry] : getStore (song).pluginValues)
        if (key.device == id && entry.isSongs)
            values.push_back ({ .parameter = key.parameter, .value = entry.value });

    std::sort (values.begin(), values.end(),
               [] (const PluginValueSet& a, const PluginValueSet& b) { return isPluginParameterBefore (a.parameter, b.parameter); });
    return values;
}

std::optional<float> findPluginValue (const Song& song, DeviceID id, const juce::String& parameter)
{
    return holdsPlugin (song, id) ? findPluginValue (getStore (song), id, parameter) : std::nullopt;
}

//==============================================================================
void storePluginOutsideHistory (Song& song, DeviceID id, const StoredPlugin& stored)
{
    auto* values = findValues (song);

    if (values == nullptr || ! id.isValid())
        return;

    auto next = values->deviceStore.get();
    const auto raw = id.getRaw();

    if (stored.state != nullptr)
    {
        if (stored.state->getSize() == 0)
            next.states = withoutEntry (next.states, raw);
        else if (const auto* held = next.states.find (raw); held == nullptr || ! (held->get() == *stored.state))
            next.states = next.states.set (raw, makeBytes (*stored.state));
    }

    if (stored.formatID.isNotEmpty())
        next.formatIDs = withEntry (next.formatIDs, raw, stored.formatID);

    if (stored.version.isNotEmpty())
        next.versions = withEntry (next.versions, raw, stored.version);

    if (stored.program.has_value())
        next.programs = *stored.program >= 0 ? withEntry (next.programs, raw, *stored.program) : withoutEntry (next.programs, raw);

    for (const auto& [parameter, value] : stored.values)
        next = withSongsValue (next, id, parameter, value);

    values->storeOutsideHistory (song, std::move (next));
}

void setPluginValueOutsideHistory (Song& song, DeviceID id, const juce::String& parameter, float value)
{
    auto* values = findValues (song);

    if (values == nullptr || ! holdsPlugin (song, id) || ! findPluginValue (values->deviceStore.get(), id, parameter).has_value())
        return;

    values->storeOutsideHistory (song, withSongsValue (values->deviceStore.get(), id, parameter, value));
}

void setPluginStateInStep (Song& song, DeviceID id, const juce::String& base64)
{
    const auto bytes = base64.isNotEmpty() ? readBytes (base64) : std::nullopt;
    setPluginStateInStep (song, id, bytes.has_value() ? *bytes : juce::MemoryBlock());
}

void setPluginStateInStep (Song& song, DeviceID id, const juce::MemoryBlock& state)
{
    auto* values = findValues (song);

    if (values == nullptr || ! holdsPlugin (song, id))
        return;

    auto next = values->deviceStore.get();
    const auto raw = id.getRaw();

    if (state.getSize() == 0)
        next.states = withoutEntry (next.states, raw);
    else if (const auto* held = next.states.find (raw); held == nullptr || ! (held->get() == state))
        next.states = next.states.set (raw, makeBytes (state));

    values->setDeviceStore (song, std::move (next));
}

void setPluginProgramInStep (Song& song, DeviceID id, int program)
{
    if (auto* values = findValues (song); values != nullptr && holdsPlugin (song, id))
    {
        auto next = values->deviceStore.get();
        next.programs = withEntry (next.programs, id.getRaw(), program);
        values->setDeviceStore (song, std::move (next));
    }
}

void setPluginValueSetByHand (Song& song, DeviceID id, const juce::String& parameter, float value, float held)
{
    auto* values = findValues (song);

    if (values == nullptr || ! holdsPlugin (song, id))
        return;

    // The song's first of it: what the plug-in holds written first as a held value, outside the history, so that the step's
    // Undo comes back to it (DeviceStoreValue.h: rule (a)).
    if (! findPluginValue (values->deviceStore.get(), id, parameter).has_value())
    {
        auto withHeld = values->deviceStore.get();
        withHeld.pluginValues = withHeld.pluginValues.set (makeKey (id, parameter), makeHeldPluginValue (held));
        values->storeOutsideHistory (song, std::move (withHeld));
    }

    values->setDeviceStore (song, withSongsValue (values->deviceStore.get(), id, parameter, value));
}

void removePluginValueSetByHand (Song& song, DeviceID id, const juce::String& parameter)
{
    auto* values = findValues (song);

    if (values == nullptr || ! holdsPlugin (song, id))
        return;

    // The value it had left as a held value, in the step, so that a Redo of the step gives the plug-in that value.
    if (const auto had = findPluginValue (values->deviceStore.get(), id, parameter))
    {
        auto next = values->deviceStore.get();
        next.pluginValues = next.pluginValues.set (makeKey (id, parameter), makeHeldPluginValue (*had));
        values->setDeviceStore (song, std::move (next));
    }
}

//==============================================================================
bool isChainMuted (const Song& song, DeviceID chain)
{
    return holdsChain (song, chain) && isChainMutedIn (getStore (song), chain);
}

bool isChainSoloed (const Song& song, DeviceID chain)
{
    return holdsChain (song, chain) && isChainSoloedIn (getStore (song), chain);
}

void setChainSwitch (Song& song, DeviceID chain, ChainFlag flag, bool on)
{
    auto* values = findValues (song);

    if (values == nullptr || ! holdsChain (song, chain))
        return;

    auto next = values->deviceStore.get();
    auto& flags = flag == ChainFlag::mute ? next.chainMutes : next.chainSolos;
    flags = on ? withEntry (flags, chain.getRaw(), true) : withoutEntry (flags, chain.getRaw());
    values->storeOutsideHistory (song, std::move (next));
}

void clearChainSwitchesInStep (Song& song, DeviceID chain)
{
    auto* values = findValues (song);

    if (values == nullptr || ! holdsChain (song, chain))
        return;

    auto next = values->deviceStore.get();
    next.chainMutes = withoutEntry (next.chainMutes, chain.getRaw());
    next.chainSolos = withoutEntry (next.chainSolos, chain.getRaw());
    values->setDeviceStore (song, std::move (next));
}

//==============================================================================
DeviceStore copyStoreOf (const Song& song, const std::map<std::uint64_t, std::uint64_t>& copied)
{
    return copyEntriesTo ({}, getStore (song), copied);
}

void copyWindowPlaces (Song& song, const std::map<std::uint64_t, std::uint64_t>& copied)
{
    auto* values = findValues (song);

    if (values == nullptr)
        return;

    for (const auto& [original, copy] : copied)
        if (const auto window = values->devicesOutside.get().findWindow (DeviceID::fromRaw (original)))
            if (findHeld (song, DeviceID::fromRaw (original)) != nullptr)
                values->setWindowPlace (DeviceID::fromRaw (copy), window);
}

void setMadeStore (Song& song, const std::vector<DeviceID>& made, const DeviceStore& store)
{
    auto* values = findValues (song);

    if (values == nullptr)
        return;

    // Which of the made the store holds entries of already, of any kind (a held value too): one walk of the plug-ins' values,
    // only where a look in the other maps found none.
    const auto& now = values->deviceStore.get();
    std::vector<bool> known (made.size(), false);
    auto anyUnknown = false;

    for (size_t i = 0; i < made.size(); ++i)
    {
        const auto raw = made[i].getRaw();
        known[i] = now.states.find (raw) != nullptr || now.programs.find (raw) != nullptr || now.formatIDs.find (raw) != nullptr
                   || now.versions.find (raw) != nullptr || now.chainMutes.find (raw) != nullptr || now.chainSolos.find (raw) != nullptr;
        anyUnknown = anyUnknown || ! known[i];
    }

    if (anyUnknown)
    {
        for (const auto& each : now.pluginValues)
            if (const auto found = std::find (made.begin(), made.end(), each.first.device); found != made.end())
                known[(size_t) std::distance (made.begin(), found)] = true;
    }

    // The entries of an item the store holds none of (a new ID: a duplicate's, a maker's, a version's device the song never
    // held) are written outside the history: unseen while the item isn't in the song (the store keeps a removed item's
    // entries and never writes, compares or counts them), they are left by the step's Undo, which takes the item out, and
    // what is written for the item later outside the history (its state stored, a value moved in its window, a copied chain
    // unsoloed) is what it holds when the step's Redo brings it back, as its element held it while the tree held the store.
    // Written in the step, the Redo would put the made entries back over those later writes. An item the store holds entries
    // of already (a version's or a merge's device come back under an ID the store knows) gets them in the step, so that its
    // Undo can put the entries it had back.
    auto outside = now;

    for (size_t i = 0; i < made.size(); ++i)
        if (! known[i])
            outside = withEntriesOf (outside, made[i], store);

    values->storeOutsideHistory (song, std::move (outside));
    auto next = values->deviceStore.get();

    for (size_t i = 0; i < made.size(); ++i)
        if (known[i])
            next = withEntriesOf (next, made[i], store);

    values->setDeviceStore (song, std::move (next));
}

//==============================================================================
DeviceStore getStoreOf (const Song& song, DeviceID id)
{
    return findHeld (song, id) != nullptr ? takeEntriesOf (getStore (song), id) : DeviceStore();
}

bool setStoreEntries (Song& song, DeviceID id, const DeviceStore& entries)
{
    auto* values = findValues (song);

    if (values == nullptr || ! (holdsPlugin (song, id) || holdsChain (song, id)))
        return false;

    const auto& now = values->deviceStore.get();
    auto next = withEntriesOf (now, id, entries);

    if (next.isSameAs (now))
        return false;

    if (next.pluginValues.identity() != now.pluginValues.identity())
        placesBuilt.fetch_add (1, std::memory_order_relaxed);

    return values->setDeviceStore (song, std::move (next));
}

std::uint64_t countPluginValuePlacesBuilt() noexcept
{
    return placesBuilt.load (std::memory_order_relaxed);
}

void forgetWindowPlace (Song& song, DeviceID id)
{
    if (auto* values = findValues (song))
        values->setWindowPlace (id, std::nullopt);
}

//==============================================================================
std::optional<WindowPlace> getWindowPlace (const Song& song, DeviceID id)
{
    const auto* values = song.getValues();
    return values != nullptr && findHeld (song, id) != nullptr ? values->devicesOutside.get().findWindow (id) : std::nullopt;
}

void setWindowPlace (Song& song, DeviceID id, WindowPlace place)
{
    if (auto* values = findValues (song); values != nullptr && findHeld (song, id) != nullptr)
        values->setWindowPlace (id, place);
}
} // namespace phazite
