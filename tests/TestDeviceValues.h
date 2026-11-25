// SPDX-License-Identifier: AGPL-3.0-or-later
// Devices, containers and chains made, read and written by the tests by the song and IDs, through the model, as TestClips.h does
// for clips: the song holds its devices as values, so what a test builds, reads or writes goes through these, and what it reads
// as a file holds it it reads from a whole copy (copyDevice()).
#pragma once

#include "TestHelpers.h"
#include "devices/core/DeviceInfo.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/mixer/DeviceType.h"

#include <functional>
#include <string_view>
#include <utility>
#include <vector>

namespace phazite::test
{
// Values set by hand of one of Phazite's own devices, by their parameters' IDs (a choice's by its number).
using OwnValues = std::vector<std::pair<juce::String, float>>;

// One of Phazite's own devices (or a type the tests registered, TestDevices.h) put on a channel at a place among its devices
// (-1: after them; an instrument goes first), with values set by hand, in the caller's undo step, as a model function adds
// one: its ID, REQUIRE'd valid.
DeviceID addOwnDevice (Song&, TrackID channel, std::string_view typeID, const OwnValues& values = {}, int index = -1);

// One of Phazite's own instruments as a track's instrument, in place of the one it had, in the caller's step: its ID,
// REQUIRE'd valid.
DeviceID setOwnInstrument (Song&, TrackID, std::string_view typeID, const OwnValues& values = {});

// One of Phazite's own devices put in a container's chain at a place (-1: after its devices; an instrument first), in the
// caller's step: its ID, REQUIRE'd valid.
DeviceID addOwnDeviceToChain (Song&, DeviceID chain, std::string_view typeID, int index = -1);

// The three above without the REQUIRE: an invalid ID where the song refuses the device (an instrument on a channel that holds
// none, an effect in an instrument's place), for what tries an edit wherever it lands and goes on (RandomEdits.h).
DeviceID tryAddOwnDevice (Song&, TrackID channel, std::string_view typeID, const OwnValues& values = {}, int index = -1);
DeviceID trySetOwnInstrument (Song&, TrackID, std::string_view typeID, const OwnValues& values = {});
DeviceID tryAddOwnDeviceToChain (Song&, DeviceID chain, std::string_view typeID, int index = -1);

// A plug-in by its description put on a channel at a place (-1: after its devices; an instrument first), in the caller's
// step, as the plug-in picker makes it: its ID, REQUIRE'd valid. What plays it loads it in the background.
DeviceID addPluginDevice (Song&, TrackID channel, const juce::PluginDescription&, int index = -1);

// The same with the plug-in's state (Schema.h: pluginState) it comes in with, in the one action that puts it in, as a plug-in
// added from settings it already has comes in.
DeviceID addPluginDevice (Song&, TrackID channel, const juce::PluginDescription&, const juce::MemoryBlock& state, int index = -1);

// One of Phazite's own devices as the browser and the menus offer it (Mixer.h: getDeviceTypes()), by its type ID,
// REQUIRE'd offered: for the model's functions that take a type (chooseInstrument(), addEffect(), addReturnWith()).
DeviceType findOwnDeviceType (std::string_view typeID);

// One of Phazite's own effects on a new return track, with a send to it from a track (as the device menus' "Add Reverb
// Return" makes one, Mixer.h: addReturnWithEffect()), in the caller's step: the return, REQUIRE'd valid.
TrackID addReturnWithOwnEffect (Song&, TrackID sender, std::string_view typeID);

//==============================================================================
// What the song holds of a device, a container or a chain as its element, written whole (model/base/internal/
// WholeState.h): what a test reads a property, a VALUES, a VALUE or a SOUNDS of. An invalid tree for one the song hasn't.
// A copy: what a test writes into it changes nothing in the song. And whether the song holds it.
juce::ValueTree copyDevice (const Song&, DeviceID);
bool hasDevice (const Song&, DeviceID);

// The song whole (copyWholeSong()) with a device's element made by hand put on a channel after its devices (a track's by its
// ID, the master's by its or none), or with a device's element, a container's or a chain's as the song holds it changed by
// hand: what a test opens (model/project/internal/Project.h: makeSong(), or TestSong::openSong()) for a song that holds what
// no model function makes (a plug-in's element another program wrote, a sidechain the model offers no device of its kind),
// as a song opens any file. A live song's own elements are never written so.
juce::ValueTree copyWholeSongWithDevice (const Song&, TrackID channel, const juce::ValueTree& device);
juce::ValueTree copyWholeSongWithDeviceEdited (const Song&, DeviceID, const std::function<void (juce::ValueTree&)>& edit);

// A parameter's value set by hand written into an element made or edited by hand (a device's or a modulator's, never a live
// song's), into its VALUES by the parameter's ID as the song writes one (SavedValues.h: toSavedValue(), a choice by its
// name), or none at the parameter's default (and no VALUES left empty).
void writeValueSetByHand (juce::ValueTree element, const devices::ParameterInfo&, float value);

// A device's type as the song holds it (Schema.h: deviceType; deviceIsPlugin for a plug-in), none for a container, a chain
// or what the song hasn't.
juce::String getDeviceTypeOf (const Song&, DeviceID);

//==============================================================================
// What a plug-in's store, or the plug-in's own window, writes into the song outside its undo history, for the tests that
// write it by hand as either does: the plug-in's state, into the song's devices' store by the device's ID, as its instance
// stores it (model/plugins/internal/DeviceStore.h: storePluginOutsideHistory()); as bytes, or as base 64 of some bytes.
void storePluginState (Song&, DeviceID, const juce::MemoryBlock&);
void storePluginState (Song&, DeviceID, const juce::String& base64);

// A plug-in's state of some bytes given by the song in the caller's undo step, as a preset loaded into it gives one
// (Presets.h), through the model's (model/plugins/internal/DeviceStore.h).
void setPluginStateInStep (Song&, DeviceID, const juce::MemoryBlock&);

// One of a device's sounds (Schema.h: SOUNDS) written outside the history, as a sound file gone since the song took it.
void writeDeviceSoundByHand (Song&, DeviceID, const juce::Identifier& property, const juce::String& value);
} // namespace phazite::test
