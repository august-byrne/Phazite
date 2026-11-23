// SPDX-License-Identifier: AGPL-3.0-or-later
// The devices' area of the song's values (roadmap step S5.20; SongValues.h, whose recipe this follows), as far as it is built:
// the song's devices, containers and chains are still the song's tree's (Schema.h: DEVICE, CONTAINER and CHAIN under a
// TRACK or the MASTER, with a DEVICE's VALUES and SOUNDS), and their values and the conversion are DevicesValue.h's. Here:
// the devices' part of the keeper, which tells the values' listeners what the tree tells of them (Listener::devicesChanged());
// and what the unsaved check compares of them with the saved copy while the trees hold them.
//
// What is the devices' and what isn't, written once (DevicesArea.cpp: isDeviceStoreProperty()): everything at or under a
// DEVICE, a CONTAINER or a CHAIN of the song's, but a plug-in's VALUE children, its state, program, format ID and version, a
// chain's mute and solo (the keyed store's, DeviceStoreArea.h, which the song's tree holds none of but for what its keeper
// takes out of an element that comes in with it) and a window's place (the value outside the history's). Message thread,
// but for the comparisons' readers, which read only the trees they are given.
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/SongValues.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace phazite
{
class Song;

// The devices' part of the keeper (SongValues.h), while the song's tree holds the devices: for each of the tree's
// notifications at or under the song's devices, containers and chains (isHeardAsDevices()), for a track given another ID or
// made a folder or a plain track again, and for each TRACK, MASTER or track list that comes into the song or leaves it with
// devices in it, whatever the history does (an undo's and a redo's, a write's outside the history), the values' listeners
// told at once, in one call, each device, container and chain it changed (Listener::devicesChanged()), so that a value set by
// hand plays in the same call, as the tree's callbacks played it: exactly what differs between the song's devices as the
// listeners were last told them (told: at the start, every one the song holds read once, as readHeldDevice() reads it, which
// no one is told) and as the song holds them now. How a notification is worked out, by its cost:
//   - A property of a device's VALUES or SOUNDS (a knob's drag, a pad's sound): that one value read from that one property
//     (SavedValues.h: readHeldSavedValue(); a sound as it is), set in the value told before (immer: a path copied), the device
//     read no more (countDevicesReadAsValues() counts none) and the song walked nowhere (countDeviceWalks() none). Only of the
//     element the value was read from (elements), its first VALUES or SOUNDS: another of the same ID the song doesn't read.
//   - Another property of a device, a container or a chain that the value covers (DevicesValue.h): its own properties read
//     again without its values and sounds (readHeldDeviceFields()), which it keeps from the value told before. Its type: read
//     whole, as another type holds other values.
//   - A device's VALUES or SOUNDS added or taken out: the device read whole again (readHeldDevice()).
//   - A device, a container or a chain added to its owner (a channel, a container, a chain), taken out of it or moved among
//     its owner's others: the DEVICE, CONTAINER and CHAIN elements of the one channel (a TRACK, the MASTER) that owner is on
//     walked as readDevicesValue() walks a channel's (each element's ID alone read; counted as a walk, countDeviceWalks(), and
//     each element looked at, countDeviceItemsWalked()), each read whole that the listeners didn't know or that is another
//     element than before, each come to another place given it, each the listeners were told on that channel and that went
//     told taken out, and each owner there whose list holds those it held before in another order told reordered: the work
//     that channel's devices make, never the song's (a drag of a device within its chain makes one at each step).
//   - Where that channel can't be counted on to say it all: a device given another ID or a type that wasn't read; a track
//     given another ID, made a folder or a plain track; a track, the master or the track list that comes or goes with
//     devices in it; an element on no channel the song reads; an item kept from an element the song still holds elsewhere,
//     or an ID found twice; and while the song holds an ID, or a channel's track's ID, twice (shadowed: which is the first,
//     only the song's order says): the song's elements walked so, each of its channels in the song's order (a folder before
//     the tracks in it, then the master), the first of an ID, a type Phazite plays.
// The calls are the tree's (one for each of its notifications that changes what the value covers, none for one that changes
// nothing of it), so that what a step told in all, not how many calls, is what holds from the devices' switch on. Nothing of the
// song's values is read.
struct SongValues::DevicesKeeper
{
    DevicesKeeper (SongValues&, Song&);

    // The keeper's callbacks, each for every element, whoever's it is, in an undo or a redo too: nothing, at once, but for
    // what is the devices'.
    void propertyChanged (const juce::ValueTree& tree, const juce::Identifier& property);
    void childAdded (const juce::ValueTree& parent, const juce::ValueTree& child);
    void childRemoved (const juce::ValueTree& parent, const juce::ValueTree& child);
    void childOrderChanged (const juce::ValueTree& parent);

    SongValues& values;
    Song& song;
    SongDevices told {}; // the song's devices as the listeners were last told them (at the start: as the song holds them)

    // Each of told's items' element, by its ID, and the walk that last found it there (settle()).
    struct Kept
    {
        juce::ValueTree element {};
        std::uint64_t seenIn = 0;
    };

    std::unordered_map<std::uint64_t, Kept> elements {};
    std::uint64_t walks = 0;
    bool shadowed = false; // the last walk of the song found an ID, or a channel's track's ID, twice: every walk the song's

    // What one notification changed, told in one call as it ends.
    struct Heard
    {
        std::vector<DeviceChange> changes {};
        std::vector<DevicePlace> reordered {};
    };

    // Each owner's list found by a walk, by its kind and its key (a channel's, a container's, a chain's), with its place and
    // its IDs in its element's order.
    using Lists = std::map<std::pair<int, std::uint64_t>, std::pair<DevicePlace, std::vector<DeviceID>>>;

    // A channel the song's walk reads (a TRACK of its track list or of a folder there, by an ID the song can count on; its
    // MASTER), with its place; and an item a channel's walk found: its element and place, and its value where the listeners
    // are told of it (read whole, or come to another place).
    struct Channel
    {
        juce::ValueTree element {};
        DevicePlace place {};
    };

    struct Found
    {
        DeviceID id {};
        juce::ValueTree element {};
        DevicePlace place {};
        std::optional<DeviceValue> taken {};
    };

    const DeviceValue* findKept (const juce::ValueTree& element) const; // told's value of what the element is, or nullptr
    void take (const DeviceValue& was, DeviceValue now, Heard&);        // one item changed in place, or come to another place
    void rereadFields (const juce::ValueTree& element, Heard&);
    void rereadWhole (const juce::ValueTree& device, Heard&);
    void rereadValue (const juce::ValueTree& device, const juce::ValueTree& values, const juce::Identifier& parameter, Heard&);
    void rereadSound (const juce::ValueTree& device, const juce::ValueTree& sounds, const juce::Identifier& property, Heard&);
    void forget (const juce::ValueTree& element);           // read again by the next walk, as one the value wasn't read from
    void settle (Heard&);                                   // the song's elements walked
    void settleIn (const juce::ValueTree& element, Heard&); // the elements of the channel an element is on, or the song's
    std::optional<Channel> findChannelOf (const juce::ValueTree& element) const;
    void finish (Heard&, SongDevices next, std::vector<DeviceValue>& taken, const Lists&); // a walk's items and lists, told
    static void addToList (Lists&, const juce::ValueTree& element, const DevicePlace&, DeviceID);
    void tell (Heard&, const SongDevices& before) const;

    JUCE_DECLARE_NON_COPYABLE (DevicesKeeper)
};

// The place of the list an item at a place is in, as SongDevices::findList() names one (its owner): a chain's for a device in
// one, a container's for a chain, else its channel's.
DevicePlace getDeviceListPlace (const DevicePlace&, DeviceValue::Form);

// What the unsaved check compares of the devices with the saved copy (model/project/Project.cpp) while the song's tree and the
// saved copy's hold them, each tree indexed by one walk, however many devices it compares (indexDeviceElementsIn()): its
// devices', containers' and chains' elements by their IDs, each with its place (the first that readDevicesValue() reads; an
// invalid tree for none: find()); and each owner's list in its element's order (a channel's devices and containers, a
// container's chains, a chain's devices: DevicePlace, as SongDevices::findList() names one), those readDevicesValue() reads,
// empty for an owner the tree hasn't (getOrder()). Reading only the tree given, each element's ID and its own properties alone
// (readHeldDeviceFields()), no value whole.
struct DeviceElements
{
    struct Found
    {
        juce::ValueTree element {};
        DevicePlace place {};
    };

    Found find (DeviceID) const;
    const std::vector<DeviceID>& getOrder (const DevicePlace& owner) const;

    std::unordered_map<std::uint64_t, Found> items {};
    std::map<std::pair<int, std::uint64_t>, std::vector<DeviceID>> lists {};
};

DeviceElements indexDeviceElementsIn (const juce::ValueTree& song);

// How many DEVICE, CONTAINER and CHAIN elements the walk above and the keeper's (DevicesKeeper::settle(), settleIn()) looked
// at, by every song since the program started: for the speed gates (tests/PerformanceGateTests.cpp), which count what an edit
// and the unsaved check walk. Any thread.
std::uint64_t countDeviceItemsWalked() noexcept;
} // namespace phazite
