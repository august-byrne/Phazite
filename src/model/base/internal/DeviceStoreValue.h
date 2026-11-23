// SPDX-License-Identifier: AGPL-3.0-or-later
// What the song stores of its devices that is written both in its undo history and outside it, as a keyed value (roadmap
// step S5.20, the devices' area; ValueArea.h, KeyedAreaValue): a plug-in's values set by hand (Schema.h: its VALUE children)
// by its device and the plug-in's own ID of the parameter, its state, program, format ID and version (pluginState,
// pluginProgram, pluginFormatID, pluginVersion), and a chain's mute and solo (chainMute, chainSolo), each an entry by its
// own key, so that an Undo puts back only the entries its step changed, as the tree undoes one property at a time: a value
// the plug-in's own window set, a state a save stored and a chain soloed after a step all survive that step's Undo (the
// devices' plain value, DevicesValue.h, covers the rest of a device; a window's place is outside the history alone,
// DevicesOutside.h). With the conversion between those properties and children of a DEVICE and a CHAIN and the store.
//
// The store is the storage: the song holds an area of it (SongValues.h: deviceStore, keyed) and its tree's DEVICE and CHAIN
// elements hold none of these properties and no VALUE child (DeviceStoreArea.h: the keeper takes in what comes with one).
// The functions by ID that read and write it are in model/plugins/internal/DeviceStore.h. Its own header, beside the other
// areas' (one header for each area's values); immer is the model's alone (tools/check-architecture.sh: immer-outside-model).
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/DevicesOutside.h"
#include "model/base/internal/Immer.h"
#include "model/base/internal/MapChanges.h"
#include "model/base/internal/ValueArea.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace phazite
{
// A plug-in's parameter by its device and the plug-in's own ID for it (Schema.h: VALUE, valueParameter), hashed for immer.
struct PluginValueKey
{
    DeviceID device {};
    juce::String parameter {};

    bool operator== (const PluginValueKey&) const = default;
};

struct PluginValueKeyHash
{
    std::size_t operator() (const PluginValueKey&) const noexcept;
};

// A plug-in's value set by hand (VALUE: valueSetByHand, 0 to 1), the song's; or, not the song's (isSongs false), what the
// plug-in held as the song first set that parameter, kept so that an Undo of that first set gives it back to the plug-in:
// never written, compared as the song's or shown.
struct PluginValueEntry
{
    float value = 0.0f;
    bool isSongs = true;

    // Written out, not defaulted: a defaulted one would compare the value with a floating-point ==, which GCC's -Wfloat-equal
    // refuses. The value compared exactly, as it is saved.
    bool operator== (const PluginValueEntry&) const noexcept;
};

// A plug-in's stored bytes (pluginState), shared between the values that hold them: compared by identity first, then by
// their content. None (no bytes) for none.
struct PluginBytes
{
    std::shared_ptr<const juce::MemoryBlock> bytes {};

    // The bytes, an empty block for none.
    const juce::MemoryBlock& get() const noexcept;

    bool operator== (const PluginBytes&) const noexcept;
};

// The rules the song's area keeps (DeviceStoreArea.h):
// (a) A plug-in's held value (an entry with isSongs false) is written outside the history (a parameter's first value set by
//     hand: model/plugins/internal/DeviceStore.h, setPluginValueSetByHand()), but for the one a value set back to its default
//     leaves in the step that takes the song's away (removePluginValueSetByHand()), so that a Redo of that step gives the
//     plug-in the step's value; and a write outside the history is compared with what the area holds by isSameAs(), never by
//     == (which sees the song's entries alone, and decides what counts as a change: a held entry written is none, but must
//     not be lost as one equal).
// (b) A removal keeps the removed device's entries: an answer written by ID for a device a step took away is there again
//     after the step's Undo (tests/DeviceStoreValueTests.cpp, sequence e).
// (c) What hears it hears it from the values' listener (SongValues.h: Listener::deviceStoreChanged()), the plug-ins'
//     instances among them (model/plugins/internal/PluginInstances.h: takeFromSong(), takeValueFromSong()).
// (d) What a file loses at its first save from the store (keepStoreAsHeld(), below): VALUEs in an order not sorted by their
//     parameters (written sorted), a chain's switch written off (mute="0", solo="0": none), a state that is not base 64 and
//     an empty format ID or version (none of either), a store's property or a VALUE on a device that isn't a plug-in, and a
//     window's place without its left (none).
// (e) PluginInstances::takeAnswers() tells the song's device an answer is for by the identity of the store's state entry as
//     it was when the answer was asked for (PluginBytes, compared by its pointer): the Undo of the device's removal brings
//     back that same entry, a version applied brings other bytes. Never an instance check (an answer that comes after a
//     removal and its Undo would be lost: the Undo opens another instance), nor an ID check (an old answer would overwrite
//     the settings a version brought in under the same ID).
//
// What the song stores of its devices, in the history and outside it, each entry by its own key: a plug-in's values set by
// hand, by device and parameter (the plug-in's held ones too: PluginValueEntry); and by a device's ID its state (none for
// none: no empty one), its program (for a plug-in with programs), its format ID and its version (none for none: no empty
// text); and by a chain's ID its mute and its solo (true alone: none for off). Entries of items the song doesn't hold (a
// device a step took away, which its Undo may bring back, and which an answer of its plug-in's may have been written for)
// are kept, and never written into a song that doesn't hold the item (writeStoreInto() writes one item's).
struct DeviceStore
{
    static constexpr bool undoesByEntry = true;

    immer::map<PluginValueKey, PluginValueEntry, PluginValueKeyHash> pluginValues {}; // VALUE
    immer::map<std::uint64_t, PluginBytes> states {};                                 // pluginState
    immer::map<std::uint64_t, int> programs {};                                       // pluginProgram
    immer::map<std::uint64_t, juce::String> formatIDs {};                             // pluginFormatID
    immer::map<std::uint64_t, juce::String> versions {};                              // pluginVersion
    immer::map<std::uint64_t, bool> chainMutes {};                                    // chainMute
    immer::map<std::uint64_t, bool> chainSolos {};                                    // chainSolo

    // The same maps without a look inside (ValueArea.h); and the same song's entries, compared (a plug-in's held values,
    // which aren't the song's, left out) by the entries that differ alone (immer's diff, which skips what the two share): a
    // write that changes one entry of a big store compares that one.
    bool isSameAs (const DeviceStore&) const noexcept;
    bool operator== (const DeviceStore&) const;
};

// The entries that differ between two stores, a list for each map (MapChanges.h); and a store with exactly those moved back
// or forward: the keyed value's functions (ValueArea.h).
struct DeviceStoreChanges
{
    std::vector<MapChange<PluginValueKey, PluginValueEntry>> pluginValues {};
    std::vector<MapChange<std::uint64_t, PluginBytes>> states {};
    std::vector<MapChange<std::uint64_t, int>> programs {};
    std::vector<MapChange<std::uint64_t, juce::String>> formatIDs {};
    std::vector<MapChange<std::uint64_t, juce::String>> versions {};
    std::vector<MapChange<std::uint64_t, bool>> chainMutes {};
    std::vector<MapChange<std::uint64_t, bool>> chainSolos {};
};

DeviceStoreChanges findChanges (const DeviceStore& before, const DeviceStore& after);
DeviceStore withChanges (DeviceStore now, const DeviceStoreChanges&, ChangeDirection);

// A plug-in's held value as the song first set the parameter (PluginValueEntry, not the song's), and a value of the song's.
PluginValueEntry makeHeldPluginValue (float value);
PluginValueEntry makeSongsPluginValue (float value);

// How many of the stores' entries the comparisons of the song's entries looked at (DeviceStore's ==, holdSameEntriesOf(): each
// entry that differs between the two), by every song since the program started: for the tests (tests/PerformanceGateTests.cpp),
// which check that a plug-in's report from its window and a drag event of a plug-in's control compare as many on a song of many
// plug-ins as on one of few. Any thread.
std::uint64_t countStoreEntriesCompared() noexcept;

//==============================================================================
// One item's entries (a plug-in's, a chain's, by its ID), each a look in the maps (a plug-in's values a walk of the values,
// which a song holds few of):
//   - findPluginValue(): a plug-in's value of a parameter, the song's alone (none for a held one), and findPluginEntry(),
//     its entry whatever it is (a held one too);
//   - isChainMutedIn(), isChainSoloedIn(): a chain's switches;
//   - takeEntriesOf(): a store of one item's entries alone (the song's alone, or the held values too);
//   - withEntriesOf(): a store with one item's entries made what entries hold of it (of that item alone: those of others
//     left out), the song's values replaced, a held value kept where the entries set none of its parameter; the very store
//     where it holds them so;
//   - holdSameEntriesOf(): whether two stores hold the same of one item, the song's entries alone (held values aside), its
//     plug-in's values by those that differ between the two (immer's diff), no walk;
//   - copyEntriesTo(): a store with what another holds of the items copied (by ID before, copied: by ID after), under their
//     copies' IDs, the song's alone (none of a held value).
std::optional<float> findPluginValue (const DeviceStore&, DeviceID, const juce::String& parameter);
const PluginValueEntry* findPluginEntry (const DeviceStore&, DeviceID, const juce::String& parameter);
bool isChainMutedIn (const DeviceStore&, DeviceID chain);
bool isChainSoloedIn (const DeviceStore&, DeviceID chain);
DeviceStore takeEntriesOf (const DeviceStore&, DeviceID, bool heldToo = false);
DeviceStore withEntriesOf (const DeviceStore&, DeviceID, const DeviceStore& entries);
bool holdSameEntriesOf (const DeviceStore&, const DeviceStore&, DeviceID);
template <typename CopiedMap>
DeviceStore copyEntriesTo (DeviceStore into, const DeviceStore& from, const CopiedMap& copied)
{
    // Each map's entries of an item copied, under its copy's ID.
    const auto copy = [&copied] (const auto& entries, auto& to)
    {
        for (const auto& [key, entry] : entries)
            if (const auto found = copied.find (key); found != copied.end())
                to = to.set (found->second, entry);
    };

    copy (from.states, into.states);
    copy (from.programs, into.programs);
    copy (from.formatIDs, into.formatIDs);
    copy (from.versions, into.versions);
    copy (from.chainMutes, into.chainMutes);
    copy (from.chainSolos, into.chainSolos);

    for (const auto& [key, entry] : from.pluginValues)
    {
        if (const auto found = copied.find (key.device.getRaw()); found != copied.end() && entry.isSongs)
        {
            auto renamed = key;
            renamed.device = DeviceID::fromRaw (found->second);
            into.pluginValues = into.pluginValues.set (renamed, entry);
        }
    }

    return into;
}

//==============================================================================
// The conversion: the one place that reads and writes the store's properties and a plug-in's VALUE children by name. Numbers
// as the song reads one (SongText.h: readValueNumber(), readDescribed()).

// What a plug-in's DEVICE or a CHAIN holds of the store, put into a store under its own ID (DevicesValue.h:
// readDeviceElementID()), as Phazite's code always read it (model/plugins/PluginInstances.cpp, model/devices/Parameters.cpp,
// model/mixer/Containers.cpp): its state's bytes from base 64 (none for none, text that isn't base 64 and no bytes); its
// program as an int reads it, none for none (one below 0 kept as it is: what reads it takes it as 0); its format ID and
// version as their text, none for none and an empty one; each VALUE by its parameter's text, its value as a float reads it
// (0 for none), the first of two of one parameter (which the loader leaves none of), each the song's; a chain's mute and
// solo each where it reads as true. Nothing for any other element, nor for one without an ID.
void readStoreOf (const juce::ValueTree& element, DeviceStore&);

// The same as the song holds it, which everything that enters the store will read (the adoption, the keeper, a version's
// devices and a merge's choice): the same as readStoreOf() but each VALUE's value by its description (Schema.h:
// valueSetByHand, within 0 and 1, 0 for what is no finite number).
void readHeldStoreOf (const juce::ValueTree& element, DeviceStore&);

// The store's entries of one item written into its element, which holds none of them (DevicesValue.h: writeDeviceElement()
// writes one so), as Phazite's writers write them (model/plugins/PluginInstances.cpp: write(); model/devices/Parameters.cpp;
// model/mixer/Containers.cpp): of a DEVICE, its state in base 64 (none for none), its format ID, its version and its program,
// each only where the store holds one, then its VALUEs, each the song's alone (never a held one), sorted by their parameters
// (numbers as numbers: "2" before "10"), each made with its parameter, then its value; of a CHAIN, its mute and solo, each
// only while on. Nothing for an element of another type, nor for an item the store holds nothing of.
void writeStoreInto (juce::ValueTree& element, const DeviceStore&, DeviceID);

// A plug-in's VALUE children's parameters in the order writeStoreInto() writes them: whether one goes before another. By
// their text's natural order (juce::String::compareNatural(): the numbers in it as numbers, "2" before "10", "a9" before
// "a10"), and where that finds two the same ("02" and "2"), by their text. A minus sign is text there, not a number's
// sign: "-2" goes before "-10", as "x2" before "x10". An order all the same: strict and total, no two different parameters
// the same both ways (tests/DeviceStoreValueTests.cpp).
bool isPluginParameterBefore (const juce::String& a, const juce::String& b);

// A device's window's place as its element holds it (Schema.h: deviceWindowX, deviceWindowY): its left and top as whole
// numbers, a top of 0 for none; none for an element without a left. And written into an element (both, as whole numbers).
std::optional<WindowPlace> readWindowPlaceOf (const juce::ValueTree& device);
void writeWindowPlaceInto (juce::ValueTree& device, WindowPlace);

// Whether a DEVICE or a CHAIN holds anything of the store's or a window's place (a store's property, a VALUE child, a window's
// left or top); and all of it taken out of it, with an undo manager or none (the keeper's, in the mode of the moment): the
// store's properties and the window's of a DEVICE and its VALUE children, a CHAIN's mute and solo. Nothing for another element.
bool holdsStoreIn (const juce::ValueTree& element);
void takeStoreOutOf (juce::ValueTree element, juce::UndoManager*);

// A DEVICE or a CHAIN of a file being opened made to hold what the song holds of the store and the window's place, as Phazite
// writes it (model/project/internal/Project.h: makeConsistent(); the one rule, readHeldStoreOf()'s): what it holds read as
// the song holds it, taken out and written back (writeStoreInto(), writeWindowPlaceInto()), the store's under its own ID. So a
// plug-in's VALUEs come sorted by their parameters after its other children, a chain's switch written off goes, a state that
// isn't base 64 of some bytes and an empty format ID or version go, and nothing of the store stays on a device that isn't a
// plug-in. An element written so already, as every file Phazite wrote holds them, is left as it is, found so without an
// allocation (isStoreWrittenAsHeld()). Whether it changed it.
bool keepStoreAsHeld (juce::ValueTree element);
bool isStoreWrittenAsHeld (const juce::ValueTree& element);
} // namespace phazite
