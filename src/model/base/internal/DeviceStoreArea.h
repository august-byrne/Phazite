// SPDX-License-Identifier: AGPL-3.0-or-later
// The devices' store's area of the song's values (roadmap step S5.20; SongValues.h, whose recipe this follows): what the
// song's devices store in the history and outside it alike (DeviceStoreValue.h: a plug-in's values set by hand, its state,
// program, format ID and version; a chain's mute and solo), held in a keyed area (an Undo puts back only the entries its step
// changed: ValueArea.h), and each device's window's place, held outside the history (DevicesOutside.h, an OutsideValue). Its
// parts, each of which a list of SongValues.h's or WholeState.h's asks in turn, a line for each area. Its value and the
// conversion of its properties and VALUE elements are DeviceStoreValue.h's. It holds no IDs of its own: its entries are by
// the IDs of the devices and chains, which the devices hold. Message thread, but for whole state's parts and the
// comparisons, which read only the values they are given.
//
// An area of properties and children of elements that stay in the song's tree (a DEVICE, a CHAIN), each entry by its item's
// ID: its keeper takes in what comes with one, and keeps an item's entries when the item goes (a removal's Undo finds them;
// an answer of a plug-in a step took away is written for it by ID), never writing nor comparing those of an item the song
// doesn't hold; they are dropped as the song is saved and opened again.
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/DeviceStoreValue.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"

#include <juce_data_structures/juce_data_structures.h>

namespace phazite
{
class Song;

// What the values' listeners are told of the store (Listener::deviceStoreChanged()): once for each change of its value, each
// entry that differs between the two (MapChanges.h, through immer's diff, which skips what the two share), each with its
// item's place as the devices' area holds it (SongValues.h: devices; no read of the tree), and the area's
// whole value after (cheap: it shares what it holds). It listens to the store's area from its making to its end.
struct SongValues::DeviceStoreTeller final : ValueArea<DeviceStore>::Listener
{
    DeviceStoreTeller (SongValues&, juce::ListenerList<SongValues::Listener>&);
    ~DeviceStoreTeller() override;

    SongValues& values; // what the devices' keeper told, each item's place
    ValueArea<DeviceStore>& area;
    juce::ListenerList<SongValues::Listener>& listeners;

    void valueAreaChanged (const DeviceStore& before, const DeviceStore& after) override;

    JUCE_DECLARE_NON_COPYABLE (DeviceStoreTeller)
};

// The store's part of the keeper (SongValues.h), in the mode of the moment, nothing while the history undoes or redoes:
//   (a) a DEVICE, a CONTAINER, a CHAIN, a TRACK, the MASTER or the track list that comes into the song with what the store
//       holds in a DEVICE or a CHAIN in it (a track a merge's choice brings in whole, a test's: DeviceStoreValue.h,
//       holdsStoreIn()): each such item's entries read as the song holds them (readHeldStoreOf()), made the item's in the
//       store (its song's entries replaced: the element is what the item holds), its window's place given, all of it taken
//       out of the element, each item counted (countDeviceStoreAdopted());
//   (b) nothing for a VALUE by itself: none comes, as the song's tree holds no DEVICE (the devices' part of the keeper takes
//       one out as it comes in, after (a): DevicesArea.h);
//   (c) nothing as an item leaves: the store keeps its entries (DeviceStoreValue.h, rule (b)).
// And the adoption from the loaded tree, which the song's holdValues() asks.
struct SongValues::DeviceStoreKeeper
{
    DeviceStoreKeeper (SongValues&, Song&);

    // A child added to an element of the song's tree (the keeper's callback, but while the history undoes or redoes, for every
    // element, whoever's it is): nothing, at once, but for what is the store's.
    void childAdded (juce::ValueTree& parent, juce::ValueTree& child);

    // What every DEVICE and CHAIN of the tree the loader made consistent holds of the store, into the area, and the windows'
    // places into the value outside the history, as the song holds them (each of which the loader left as it holds it:
    // asserted, DeviceStoreValue.h: isStoreWrittenAsHeld()), and all of it out of the tree (SongValues.cpp: adopt()).
    static void adopt (SongValues& into, Song& loaded);

    SongValues& values;
    Song& song;

    JUCE_DECLARE_NON_COPYABLE (DeviceStoreKeeper)
};

// The DEVICE and CHAIN elements of a channel's element (a TRACK or the MASTER that comes into the song with them: the tracks'
// safety net, TracksArea.h), in its devices and containers, what each holds of the store read in as the song holds it
// (DeviceStoreValue.h: readHeldStoreOf(): its entries made the element's) and a device's window's place. How many of them held
// any of it (DeviceStoreValue.h: holdsStoreIn()).
int takeChannelStoreIn (DeviceStore&, DevicesOutside&, const juce::ValueTree& channel);

// The store's part of whole state (WholeState.cpp: writeIn()): whether a child of an element in a copy of the song leads to a
// DEVICE or a CHAIN (the master, a channel's devices and containers, a container's chains, a chain's devices, from the song's
// element down: whole state writes each TRACK itself, WholeState.cpp: writeTracksIn(), whose devices this leads to from it);
// and a copy's DEVICE given what the store holds of its plug-in
// (DeviceStoreValue.h: writeStoreInto(), its VALUEs sorted by their parameters after its other children) and its window's
// place, a CHAIN its mute and solo, from the values kept. Nothing for another element.
bool leadsToDeviceStore (const juce::ValueTree& parent, const juce::ValueTree& child);
void writeDeviceStoreIn (juce::ValueTree& element, const ValuesKept&);

// The store's part of the comparison of two elements with what their songs kept (WholeState.h: holdSameKeptOf()): a DEVICE's
// or a CHAIN's entries, the song's alone (a held value aside; a window's place, which counts as no change, aside), the same
// in both; true for another element.
bool holdSameDeviceStoreOf (const juce::ValueTree& element, const ValuesKept&, const juce::ValueTree& otherElement, const ValuesKept&);
} // namespace phazite
