// SPDX-License-Identifier: AGPL-3.0-or-later
// The devices' area of the song's values (roadmap step S5.20; SongValues.h, whose recipe this follows): its parts, each of
// which a list of SongValues.h's, WholeState.h's or IDsHeld.h's asks in turn, a line for each area. Its value and the
// conversion of its elements are DevicesValue.h's. It holds IDs (each device's, container's and chain's). Message thread,
// but for whole state's parts and the comparisons, which read only the values they are given.
//
// The song's devices, containers and chains are the area's (ValueArea<SongDevices>, plain: undone whole): the song's tree
// holds no DEVICE, CONTAINER, CHAIN, VALUES or SOUNDS element. What is the devices' and what isn't: everything a DEVICE, a
// CONTAINER or a CHAIN of a file holds, but a plug-in's VALUE children, its state, program, format ID and version, a chain's
// mute and solo (the keyed store's, DeviceStoreArea.h) and a window's place (the value outside the history's), and a
// container's macros (modulation's, ModulationArea.h). The area sits under elements that stay in the song's tree (a TRACK,
// the MASTER), as the sends' do, and holds the rest of its owners itself (a container, a chain): so its keeper follows a
// track as it comes and goes (alone, a folder with its tracks, the track list), and nothing else.
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <vector>

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

// The devices' part of the keeper (SongValues.h), in the mode of the moment, nothing while the history undoes or redoes:
//   (a) a TRACK (alone, a folder with its tracks, the track list) taken out of the song's track list: each track's channel's
//       devices and containers, their chains and their devices remembered with its element, in their lists' order, and
//       erased; what an element was remembered with at an earlier removal (taken out, and brought back by an undo since,
//       which gave its devices back itself) replaced at every removal, or forgotten where it holds none now, so that it never
//       brings back a device removed since (as the sends' keeper does: SendsArea.cpp); the same element back: given back,
//       their places' track the element's ID then. Every function that removes a track erases its devices first
//       (model/arrangement/internal/Tracks.h: eraseTrackHoldings()), so that this is a track JUCE moves (taken out and put in
//       again in one step) alone, until the tracks' switch;
//   (b) a TRACK, a folder's tracks, the track list or the MASTER that comes into the song with DEVICE or CONTAINER elements in
//       it (a safety net: a test's; no path of Phazite's relies on it): each device, container and chain read as the song
//       holds it (DevicesValue.h: readHeldDevice(); what the store holds of it taken in before by the store's part,
//       DeviceStoreArea.h, and a container's macros by modulation's), set, taken out of the tree, and counted
//       (countDevicesAdopted()); one of an ID the song holds already dropped, the song's kept;
//   (c) a DEVICE, a CONTAINER, a CHAIN, a VALUES or a SOUNDS added by itself to an element of the song's: refused in a step (a
//       JUCE assertion: the step can't take it out again, as its Undo, taking the tree's change back first, would find it
//       gone; JUCE workaround W62 (docs/juce-workarounds.md)); outside the history, a DEVICE or a CONTAINER added to a channel
//       read in after its others there and counted, and each of them taken out.
// The store's entries stay as a device leaves (the store's own rule: DeviceStoreArea.h). And the adoption from the loaded
// tree, which the song's holdValues() asks.
struct SongValues::DevicesKeeper
{
    DevicesKeeper (SongValues&, Song&);

    // A child added to an element of the song's tree, or taken out of one (the keeper's callbacks, but while the history
    // undoes or redoes, for every element, whoever's it is): nothing, at once, but for what is the devices' (a track or the
    // track list in the song's, the master, a DEVICE, a CONTAINER, a CHAIN, a VALUES or a SOUNDS in the song's tree).
    void childAdded (juce::ValueTree& parent, juce::ValueTree& child);
    void childRemoved (juce::ValueTree& parent, juce::ValueTree& child);

    // Every device, container and chain of the tree the loader made consistent into the area, outside the history, as the song
    // holds them (each read once by readHeldDevice(), each of which the loader left as the song holds it), and every DEVICE,
    // CONTAINER and CHAIN element out of the tree, those anywhere else too (SongValues.cpp: adopt()).
    static void adopt (SongValues& into, Song& loaded);

    SongValues& values;
    Song& song;
    juce::ValueTree state;

    // A track taken out of the song with its devices, containers and chains as it went, each list's in its order, an owner
    // before what it holds: given back if the same element comes back, let go of once nothing else holds the element (the
    // history let go of the step that could bring it back) or it is back in the song by an undo, which brought them back
    // itself.
    struct Remembered
    {
        juce::ValueTree track {};
        std::vector<DeviceValue> devices {};
    };

    std::vector<Remembered> remembered {};
    size_t sweepAt = 64;

    bool isInTrackList (const juce::ValueTree& parent, const juce::ValueTree& child) const;
    bool isInSong (const juce::ValueTree& element) const;
    void sweep();

    JUCE_DECLARE_NON_COPYABLE (DevicesKeeper)
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
// a window's place, a container's macros), which a whole copy of it writes (WholeState.h: copyWhole()). An invalid tree for an
// ID the value hasn't.
juce::ValueTree writeDeviceWhole (const SongDevices&, DeviceID);

// The devices' part of whole state (WholeState.cpp: writeIn()): whether a child of an element in a copy of the song leads to a
// channel (the master, from the song's element down: whole state writes each TRACK itself, WholeState.cpp: writeTracksIn(),
// asking each area of each); and a copy's TRACK or MASTER given its devices and containers from the values kept (a track's by
// its ID, the master's under 0), in their list's order, each with what it holds (writeDeviceWhole()), right after its clips
// (which the clips' part writes first among its children) and before its other children. Nothing for another element.
bool leadsToDevices (const juce::ValueTree& parent, const juce::ValueTree& child);
void writeDevicesIn (juce::ValueTree& element, const ValuesKept&);

// The DEVICE and CONTAINER elements of a channel's element (a TRACK or the MASTER that comes into the song with them: the
// tracks' safety net, TracksArea.h), each with what it holds (a container's CHAINs, a chain's DEVICEs), read as the song holds
// one (DevicesValue.h: readHeldDevice()) at the channel's place given, an owner before what it holds; one of an ID the area
// holds already left out with what it holds. How many it read.
int takeChannelDevicesIn (SongDevices&, const juce::ValueTree& channel, const DevicePlace&);

// The devices' part of the comparison of two elements with what their songs kept (WholeState.h: holdSameKeptOf()): a TRACK's
// or the MASTER's devices and containers, the same in the same order, each the same value with what it holds (a container's
// chains in their order, each chain's devices in theirs); true for another element.
bool holdSameDevicesOf (const juce::ValueTree& element, const ValuesKept&, const juce::ValueTree& otherElement, const ValuesKept&);
} // namespace phazite
