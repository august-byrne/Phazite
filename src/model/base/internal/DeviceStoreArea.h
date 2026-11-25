// SPDX-License-Identifier: AGPL-3.0-or-later
// The devices' store's area of the song's values (roadmap step S5.20; SongValues.h, whose recipe this follows): what the
// song's devices store in the history and outside it alike (DeviceStoreValue.h: a plug-in's values set by hand, its state,
// program, format ID and version; a chain's mute and solo), held in a keyed area (an Undo puts back only the entries its step
// changed: ValueArea.h), and each device's window's place, held outside the history (DevicesOutside.h, an OutsideValue). Its
// parts, each of which a list of SongValues.h's or WholeState.h's asks in turn, a line for each area. Its value and the
// conversion of its properties and VALUE elements are DeviceStoreValue.h's. It holds no IDs of its own: its entries are by
// the IDs of the devices and chains, which the devices hold. Message thread, but for whole state's parts and the
// comparisons, which read only the values they are given.
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

    SongValues& values; // what the devices' area told, each item's place
    ValueArea<DeviceStore>& area;
    juce::ListenerList<SongValues::Listener>& listeners;

    void valueAreaChanged (const DeviceStore& before, const DeviceStore& after) override;

    JUCE_DECLARE_NON_COPYABLE (DeviceStoreTeller)
};

// The store's part of whole state (WholeState.cpp: writeIn()): whether a child of an element in a copy of the song leads to a
// DEVICE or a CHAIN (the master, a channel's devices and containers, a container's chains, a chain's devices, from the song's
// element down: whole state writes each TRACK itself, WholeState.cpp: writeTracksIn(), whose devices this leads to from it);
// and a copy's DEVICE given what the store holds of its plug-in
// (DeviceStoreValue.h: writeStoreInto(), its VALUEs sorted by their parameters after its other children) and its window's
// place, a CHAIN its mute and solo, from the values kept. Nothing for another element.
bool leadsToDeviceStore (const juce::ValueTree& parent, const juce::ValueTree& child);
void writeDeviceStoreIn (juce::ValueTree& element, const SongState&);

// The store's reader of a song's tree (SongState.h: readSongTree(), a line of it): what every DEVICE and CHAIN of its channels
// stores, and each device's window's place, into the state, as the song holds them; no tree changed. Any thread.
void readHeldDeviceStoreIn (const juce::ValueTree& song, SongState&);
} // namespace phazite
