// SPDX-License-Identifier: AGPL-3.0-or-later
#include "model/base/internal/DeviceStoreValue.h"
#include "model/base/Numbers.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <optional>
#include <utility>

namespace phazite
{
namespace
{
    std::atomic<std::uint64_t> entriesCompared { 0 }; // countStoreEntriesCompared(): a count alone, which orders nothing

    void countCompared() noexcept
    {
        entriesCompared.fetch_add (1, std::memory_order_relaxed);
    }
} // namespace

std::uint64_t countStoreEntriesCompared() noexcept
{
    return entriesCompared.load (std::memory_order_relaxed);
}

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
    // Whether two maps differ in an entry, from immer's diff, which skips what the two share (one made from the other: what
    // differs alone is looked at), each entry that differs counted.
    template <typename Map>
    bool haveDifferentEntries (const Map& a, const Map& b)
    {
        if (a.identity() == b.identity())
            return false;

        auto differ = false;
        const auto found = [&differ] (const auto&...)
        {
            countCompared();
            differ = true;
        };

        immer::diff (a, b, found, found, found);
        return differ;
    }

    // Whether a plug-in's value that differs between two stores (either none) differs as the song's: the song's in one alone,
    // or in both with another value (a held value is no value of the song's).
    bool differsAsSongs (const PluginValueEntry* before, const PluginValueEntry* after)
    {
        const auto songsBefore = before != nullptr && before->isSongs;
        const auto songsAfter = after != nullptr && after->isSongs;
        return songsBefore != songsAfter || (songsBefore && ! juce::exactlyEqual (before->value, after->value));
    }

    // Whether the song's values of the plug-ins differ between two stores, of one plug-in alone where one is given: from
    // immer's diff, each value that differs looked at (and counted), what the two share skipped.
    bool haveDifferentSongsValues (const DeviceStore& a, const DeviceStore& b, std::optional<DeviceID> only = std::nullopt)
    {
        if (a.pluginValues.identity() == b.pluginValues.identity())
            return false;

        auto differ = false;
        const auto look = [&differ, only] (const PluginValueKey& key, const PluginValueEntry* before, const PluginValueEntry* after)
        {
            countCompared();

            if ((! only.has_value() || key.device == *only) && differsAsSongs (before, after))
                differ = true;
        };

        immer::diff (
            a.pluginValues, b.pluginValues, [&look] (const auto& added) { look (added.first, nullptr, &added.second); },
            [&look] (const auto& removed) { look (removed.first, &removed.second, nullptr); },
            [&look] (const auto& old, const auto& now) { look (old.first, &old.second, &now.second); });
        return differ;
    }
} // namespace

bool DeviceStore::operator== (const DeviceStore& other) const
{
    // What differs alone looked at (a plug-in's report and a drag event compare one entry, however big the song).
    if (isSameAs (other))
        return true;

    return ! haveDifferentEntries (states, other.states) && ! haveDifferentEntries (programs, other.programs)
           && ! haveDifferentEntries (formatIDs, other.formatIDs) && ! haveDifferentEntries (versions, other.versions)
           && ! haveDifferentEntries (chainMutes, other.chainMutes) && ! haveDifferentEntries (chainSolos, other.chainSolos)
           && ! haveDifferentSongsValues (*this, other);
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
    PluginValueKey makeKey (DeviceID device, const juce::String& parameter)
    {
        PluginValueKey key;
        key.device = device;
        key.parameter = parameter;
        return key;
    }

    // A map's entry of a key made what another holds of it (none taken away), the very map where it holds it so already.
    template <typename Map>
    Map withEntryOf (const Map& map, const Map& from, std::uint64_t key)
    {
        const auto* held = map.find (key);
        const auto* wanted = from.find (key);

        if (wanted == nullptr)
            return held != nullptr ? map.erase (key) : map;

        return held != nullptr && *held == *wanted ? map : map.set (key, *wanted);
    }

    template <typename Map>
    bool holdSameEntry (const Map& a, const Map& b, std::uint64_t key)
    {
        const auto* x = a.find (key);
        const auto* y = b.find (key);
        return (x == nullptr) == (y == nullptr) && (x == nullptr || *x == *y);
    }
} // namespace

std::optional<float> findPluginValue (const DeviceStore& store, DeviceID id, const juce::String& parameter)
{
    if (const auto* entry = findPluginEntry (store, id, parameter); entry != nullptr && entry->isSongs)
        return entry->value;

    return std::nullopt;
}

const PluginValueEntry* findPluginEntry (const DeviceStore& store, DeviceID id, const juce::String& parameter)
{
    return store.pluginValues.find (makeKey (id, parameter));
}

bool isChainMutedIn (const DeviceStore& store, DeviceID chain)
{
    const auto* muted = store.chainMutes.find (chain.getRaw());
    return muted != nullptr && *muted;
}

bool isChainSoloedIn (const DeviceStore& store, DeviceID chain)
{
    const auto* soloed = store.chainSolos.find (chain.getRaw());
    return soloed != nullptr && *soloed;
}

DeviceStore takeEntriesOf (const DeviceStore& store, DeviceID id, bool heldToo)
{
    DeviceStore taken;
    const auto raw = id.getRaw();
    const DeviceStore none;
    taken.states = withEntryOf (none.states, store.states, raw);
    taken.programs = withEntryOf (none.programs, store.programs, raw);
    taken.formatIDs = withEntryOf (none.formatIDs, store.formatIDs, raw);
    taken.versions = withEntryOf (none.versions, store.versions, raw);
    taken.chainMutes = withEntryOf (none.chainMutes, store.chainMutes, raw);
    taken.chainSolos = withEntryOf (none.chainSolos, store.chainSolos, raw);

    for (const auto& [key, entry] : store.pluginValues)
        if (key.device == id && (heldToo || entry.isSongs))
            taken.pluginValues = taken.pluginValues.set (key, entry);

    return taken;
}

DeviceStore withEntriesOf (const DeviceStore& store, DeviceID id, const DeviceStore& entries)
{
    auto next = store;
    const auto raw = id.getRaw();
    next.states = withEntryOf (store.states, entries.states, raw);
    next.programs = withEntryOf (store.programs, entries.programs, raw);
    next.formatIDs = withEntryOf (store.formatIDs, entries.formatIDs, raw);
    next.versions = withEntryOf (store.versions, entries.versions, raw);
    next.chainMutes = withEntryOf (store.chainMutes, entries.chainMutes, raw);
    next.chainSolos = withEntryOf (store.chainSolos, entries.chainSolos, raw);

    // The song's values the entries hold none of taken away (a held one kept), each they hold set where it differs.
    for (const auto& [key, entry] : store.pluginValues)
    {
        if (key.device != id || ! entry.isSongs)
            continue;

        if (const auto* wanted = entries.pluginValues.find (key); wanted == nullptr || ! wanted->isSongs)
            next.pluginValues = next.pluginValues.erase (key);
    }

    for (const auto& [key, entry] : entries.pluginValues)
    {
        if (key.device != id || ! entry.isSongs)
            continue;

        if (const auto* held = next.pluginValues.find (key); held == nullptr || ! (*held == entry))
            next.pluginValues = next.pluginValues.set (key, entry);
    }

    return next;
}

bool holdSameEntriesOf (const DeviceStore& a, const DeviceStore& b, DeviceID id)
{
    const auto raw = id.getRaw();
    return holdSameEntry (a.states, b.states, raw) && holdSameEntry (a.programs, b.programs, raw)
           && holdSameEntry (a.formatIDs, b.formatIDs, raw) && holdSameEntry (a.versions, b.versions, raw)
           && holdSameEntry (a.chainMutes, b.chainMutes, raw) && holdSameEntry (a.chainSolos, b.chainSolos, raw)
           && ! haveDifferentSongsValues (a, b, id);
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

std::optional<WindowPlace> readWindowPlaceOf (const juce::ValueTree& device)
{
    if (! device.hasProperty (schema::deviceWindowX))
        return std::nullopt;

    return WindowPlace { .x = toWhole<int> (device[schema::deviceWindowX]), .y = toWhole<int> (device[schema::deviceWindowY]) };
}

void writeWindowPlaceInto (juce::ValueTree& device, WindowPlace place)
{
    device.setProperty (schema::deviceWindowX, place.x, nullptr);
    device.setProperty (schema::deviceWindowY, place.y, nullptr);
}

namespace
{
    // The store's properties of a DEVICE, with its window's place.
    const std::array<const juce::Identifier*, 6>& getDeviceStoreProperties()
    {
        static const std::array<const juce::Identifier*, 6> properties {
            &schema::pluginState.getName(),   &schema::pluginProgram.getName(), &schema::pluginFormatID.getName(),
            &schema::pluginVersion.getName(), &schema::deviceWindowX.getName(), &schema::deviceWindowY.getName()
        };
        return properties;
    }

    // Whether a text is base 64 of some bytes as Phazite writes it (juce::Base64::toBase64(): groups of four characters of
    // its alphabet, "=" filling the last, the bits past the bytes 0), which readBytes() reads and writeBytes() writes again
    // as the same text. Read in place, without an allocation.
    bool isWrittenBase64 (const juce::String& text)
    {
        const auto valueOf = [] (juce::juce_wchar c) -> int
        {
            if (c >= 'A' && c <= 'Z')
                return (int) (c - 'A');

            if (c >= 'a' && c <= 'z')
                return (int) (c - 'a') + 26;

            if (c >= '0' && c <= '9')
                return (int) (c - '0') + 52;

            if (c == '+')
                return 62;

            return c == '/' ? 63 : -1;
        };

        const auto length = text.length();

        if (length == 0 || length % 4 != 0)
            return false;

        auto p = text.getCharPointer();

        for (int i = 0; i < length; i += 4)
        {
            int group[4] {};

            for (auto& each : group)
                each = valueOf (p.getAndAdvance());

            const auto last = i + 4 == length;
            const auto padding = last && text.endsWith ("==") ? 2 : (last && text.endsWithChar ('=') ? 1 : 0);

            for (int k = 0; k < 4 - padding; ++k)
                if (group[k] < 0)
                    return false;

            if ((padding == 2 && (group[1] & 15) != 0) || (padding == 1 && (group[2] & 3) != 0))
                return false;
        }

        return true;
    }

    bool isWrittenWhole (const juce::var& held)
    {
        return isSameValue (held, juce::var (toWhole<int> (held)));
    }
} // namespace

bool holdsStoreIn (const juce::ValueTree& element)
{
    if (element.hasType (schema::CHAIN))
        return element.hasProperty (schema::chainMute) || element.hasProperty (schema::chainSolo);

    if (! element.hasType (schema::DEVICE))
        return false;

    for (const auto* property : getDeviceStoreProperties())
        if (element.hasProperty (*property))
            return true;

    return element.getChildWithName (schema::VALUE).isValid();
}

void takeStoreOutOf (juce::ValueTree element, juce::UndoManager* undo)
{
    if (element.hasType (schema::CHAIN))
    {
        element.removeProperty (schema::chainMute, undo);
        element.removeProperty (schema::chainSolo, undo);
        return;
    }

    if (! element.hasType (schema::DEVICE))
        return;

    for (const auto* property : getDeviceStoreProperties())
        element.removeProperty (*property, undo);

    for (int i = element.getNumChildren(); --i >= 0;)
        if (element.getChild (i).hasType (schema::VALUE))
            element.removeChild (i, undo);
}

bool isStoreWrittenAsHeld (const juce::ValueTree& element)
{
    if (element.hasType (schema::CHAIN))
    {
        for (const auto* property : { &schema::chainMute.getName(), &schema::chainSolo.getName() })
            if (const auto* held = element.getPropertyPointer (*property);
                held != nullptr && (! isSameValue (*held, true) || ! readDeviceElementID (element).isValid()))
                return false;

        return true;
    }

    if (! element.hasType (schema::DEVICE))
        return true;

    // A window's place: both, or neither, each a whole number.
    const auto* left = element.getPropertyPointer (schema::deviceWindowX);
    const auto* top = element.getPropertyPointer (schema::deviceWindowY);

    if ((left == nullptr) != (top == nullptr) || (left != nullptr && (! isWrittenWhole (*left) || ! isWrittenWhole (*top))))
        return false;

    const auto isPlugin = readDeviceForm (element) == DeviceValue::Form::plugin && readDeviceElementID (element).isValid();

    if (! isPlugin)
    {
        for (const auto* property : getDeviceStoreProperties())
            if (! schema::isDeviceViewProperty (*property) && element.hasProperty (*property))
                return false;

        return ! element.getChildWithName (schema::VALUE).isValid();
    }

    if (const auto* state = element.getPropertyPointer (schema::pluginState); state != nullptr && ! isWrittenBase64 (state->toString()))
        return false;

    if (const auto* program = element.getPropertyPointer (schema::pluginProgram); program != nullptr && ! isWrittenWhole (*program))
        return false;

    for (const auto* text : { &schema::pluginFormatID.getName(), &schema::pluginVersion.getName() })
        if (const auto* held = element.getPropertyPointer (*text); held != nullptr && held->toString().isEmpty())
            return false;

    // Its VALUEs after its other children, each its parameter and its value alone, as the song holds it, sorted strictly by
    // their parameters (none of one parameter twice).
    const juce::var* before = nullptr;
    auto inValues = false;

    for (const auto& child : element)
    {
        if (! child.hasType (schema::VALUE))
        {
            if (inValues)
                return false;

            continue;
        }

        inValues = true;
        const auto* parameter = child.getPropertyPointer (schema::valueParameter);
        const auto* value = child.getPropertyPointer (schema::valueSetByHand);

        if (parameter == nullptr || value == nullptr || child.getNumProperties() != 2 || child.getNumChildren() != 0
            || ! isSameValue (*value, juce::var ((float) readDescribed (*value, schema::valueSetByHand)))
            || (before != nullptr && ! isPluginParameterBefore (before->toString(), parameter->toString())))
            return false;

        before = parameter;
    }

    return true;
}

bool keepStoreAsHeld (juce::ValueTree element)
{
    if (isStoreWrittenAsHeld (element))
        return false;

    DeviceStore store;
    readHeldStoreOf (element, store);
    const auto window = element.hasType (schema::DEVICE) ? readWindowPlaceOf (element) : std::nullopt;
    takeStoreOutOf (element, nullptr);
    writeStoreInto (element, store, readDeviceElementID (element));

    if (window.has_value())
        writeWindowPlaceInto (element, *window);

    return true;
}
} // namespace phazite
