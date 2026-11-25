// SPDX-License-Identifier: AGPL-3.0-or-later
// The devices' area of the song's values (SongValues.h, whose recipe this follows): its parts, each of
// which a list of SongValues.h's, WholeState.h's or IDsHeld.h's asks in turn, a line for each area. Its value and the
// conversion of its elements are DevicesValue.h's. It holds IDs (each device's, container's and chain's). Message thread,
// but for whole state's parts and the comparisons, which read only the values they are given.
//
// The song's devices, containers and chains are the area's (ValueArea<SongDevices>, plain: undone whole). What is the
// devices' and what isn't: everything a DEVICE, a CONTAINER or a CHAIN of a file holds, but a plug-in's VALUE children, its
// state, program, format ID and version, a chain's mute and solo (the keyed store's, DeviceStoreArea.h) and a window's place
// (the value outside the history's), and a container's macros (modulation's, ModulationArea.h). Each item names its channel
// by its ID, and the area holds the rest of its owners itself (a container, a chain).
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>

namespace phazite
{
class Song;

// What the values' listeners are told of the devices (Listener::devicesChanged()): once for each change of its value, each
// device, container and chain whose value differs between the two (immer's diff, which skips what the two share; one whose
// value is another alike, none), with where it was and is, the parts of it that changed and its value before and after; each
// owner whose list holds those it held before in another order; and the area's whole value after (cheap: it shares what it
// holds). It listens to the devices' area from its making to its end.
struct SongValues::DevicesTeller final : ValueArea<SongDevices>::Listener
{
    DevicesTeller (ValueArea<SongDevices>&, juce::ListenerList<SongValues::Listener>&);
    ~DevicesTeller() override;

    ValueArea<SongDevices>& area;
    juce::ListenerList<SongValues::Listener>& listeners;

    void valueAreaChanged (const SongDevices& before, const SongDevices& after) override;

    JUCE_DECLARE_NON_COPYABLE (DevicesTeller)
};

// The place of the list an item at a place is in, as SongDevices::findList() names one (its owner): a chain's for a device in
// one, a container's for a chain, else its channel's.
DevicePlace getDeviceListPlace (const DevicePlace&, DeviceValue::Form);

// The place of what a channel holds: a track's by its ID, the master's by its flag; and of what a container (by its ID), or a
// chain in it, holds on a channel's place.
DevicePlace makeChannelPlace (TrackID track, bool onMaster);
DevicePlace makeContainerPlace (const DevicePlace& channel, DeviceID container, DeviceID chain = {});

// An item of the devices' value as an element with what it holds, as whole state writes one (writeDeviceElement(), a
// container's CHAINs in their order, each with its DEVICEs in theirs): without what the other areas write into it (the store's,
// a window's place, a container's macros), which a whole copy of it writes (WholeState.h: writeDeviceTree()). An invalid
// tree for an ID the value hasn't.
juce::ValueTree writeDeviceWhole (const SongDevices&, DeviceID);

// The devices' part of whole state (WholeState.cpp: writeIn()): whether a child of an element in a copy of the song leads to a
// channel (the master, from the song's element down: whole state writes each TRACK itself, WholeState.cpp: writeTracksIn(),
// asking each area of each); and a copy's TRACK or MASTER given its devices and containers from the values kept (a track's by
// its ID, the master's under 0), in their list's order, each with what it holds (writeDeviceWhole()), right after its clips
// (which the clips' part writes first among its children) and before its other children. Nothing for another element.
bool leadsToDevices (const juce::ValueTree& parent, const juce::ValueTree& child);
void writeDevicesIn (juce::ValueTree& element, const SongState&);

// The devices' reader of a song's tree (SongState.h: readSongTree(), a line of it): every device, container and chain of its
// channels into the state, as the song holds them; no tree changed. Any thread.
void readHeldDevicesIn (const juce::ValueTree& song, SongState&);

// The devices' part of the comparison of two states (WholeState.h: holdSameChannels()): a channel's devices and containers (a
// track's, the master's: makeChannelPlace()), the same in the same order, each the same value with what it holds (a container's
// chains in their order, each chain's devices in theirs) and what the store holds of each (holdSameDeviceStore()). Any thread.
bool holdSameChannelDevices (const SongState&, const SongState&, const DevicePlace& channel);
} // namespace phazite
