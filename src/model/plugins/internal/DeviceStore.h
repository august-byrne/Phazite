// SPDX-License-Identifier: AGPL-3.0-or-later
// What the song stores of its devices in its undo history and outside it alike, read and written by IDs (roadmap step S5.20,
// the devices' area; the store's value: model/base/internal/DeviceStoreValue.h): a plug-in's state, program, format ID and
// version (Schema.h: pluginState, pluginProgram, pluginFormatID, pluginVersion) and its values set by hand (its VALUE
// children in a file); a chain's mute and solo (chainMute, chainSolo); and, outside the history alone, a device's window place
// (deviceWindowX, deviceWindowY; model/base/internal/DevicesOutside.h). The one block of the model that reads and writes them
// in a live song: the plug-ins' instances (PluginInstances.h), the parameters' code (model/devices/Parameters.cpp), the
// containers' (model/mixer/Containers.cpp), a preset loaded into a plug-in (model/presets/Presets.cpp), the devices' writers
// (model/mixer/DeviceWriters.cpp: a copy's and a maker's entries), the versions (model/versions/ApplySong.cpp, a merge's
// choice: model/mixer/DeviceClashes.cpp) and the plug-in's window (src/app/PluginWindow.cpp) reach them here alone.
//
// The song's values are their storage (SongValues.h: deviceStore, a keyed area, and devicesOutside, a value outside the
// history), which the song's tree holds nothing of. Each read is a look in a map: whether the song holds the item asked of
// (a plug-in, a chain, a device) is found in the devices' area (SongValues.h: devices), so that no read walks the
// song (tests/PerformanceGateTests.cpp). The store keeps the entries of an item
// a step took away (its Undo finds them, an answer of its plug-in's is written for it by ID), which no reader here gives and
// no comparison sees. What hears the store change hears it from the values' listener (SongValues.h:
// Listener::deviceStoreChanged()). Message thread only.
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/DeviceStoreValue.h"
#include "model/base/internal/DevicesOutside.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace phazite
{
class Song;

//==============================================================================
// A plug-in's state (its settings' bytes), empty for none; the same as the store holds it, shared (its identity tells what a
// store wrote from bytes brought in another way: PluginInstances' late answers); its program, none for none, as the song
// holds it (an int, which a damaged file may make negative: a caller keeps it as it took it); its format ID and its version,
// none for none. Each nothing for a device the song hasn't, or that isn't a plug-in.
juce::MemoryBlock getPluginState (const Song&, DeviceID);
PluginBytes getPluginStateBytes (const Song&, DeviceID);
std::optional<int> getPluginProgram (const Song&, DeviceID);
juce::String getPluginFormatID (const Song&, DeviceID);
juce::String getPluginVersion (const Song&, DeviceID);

// A plug-in's parameter, by the plug-in's own ID for it (a VST3's number), with a value of it.
struct PluginValueSet
{
    juce::String parameter {};
    float value = 0.0f;
};

// A plug-in's values set by hand, each parameter with its value, sorted by their parameters (DeviceStoreValue.h:
// isPluginParameterBefore()); and one parameter's, none for one the song sets none of. Each nothing for a device the song
// hasn't, or that isn't a plug-in.
std::vector<PluginValueSet> getPluginValues (const Song&, DeviceID);
std::optional<float> findPluginValue (const Song&, DeviceID, const juce::String& parameter);

//==============================================================================
// What a plug-in's instance stores of it into the song, outside the undo history (PluginInstances: store(), its answers), by
// the device's ID whether the song holds it now or not (an answer that comes after a step took the device away is there for
// the step's Undo), each only where it differs from what the store holds: its state, set where it holds bytes and taken away
// where it is empty (none: left as it is); its format ID and its version, each set where it isn't empty; its program, set
// where it is 0 or more and taken away at -1 (one of one program or none; none: left); and the values set by hand of the
// parameters automation or modulation moves, each given to the song's value of its parameter, one made where the song has
// none. Its state is the caller's block, copied once into the store where it differs (a plug-in's state can be megabytes).
struct StoredPlugin
{
    const juce::MemoryBlock* state = nullptr;
    juce::String formatID {};
    juce::String version {};
    std::optional<int> program {};
    std::vector<PluginValueSet> values {};
};

void storePluginOutsideHistory (Song&, DeviceID, const StoredPlugin&);

// A plug-in's value set in its own window, outside the history: given to the song's value of the parameter where it holds
// one, none made (PluginInstances: listen()). Nothing for a device the song hasn't.
void setPluginValueOutsideHistory (Song&, DeviceID, const juce::String& parameter, float value);

// A plug-in's state and program given by the song in the caller's undo step, as a preset loaded into it gives them
// (model/presets/Presets.cpp: loadPresetInto()): its state, taken away where it is empty (as text in base 64, as the preset
// holds it, none for text that isn't base 64 of some bytes; or as bytes); its program. Nothing for a device the song hasn't,
// or that isn't a plug-in.
void setPluginStateInStep (Song&, DeviceID, const juce::String& base64);
void setPluginStateInStep (Song&, DeviceID, const juce::MemoryBlock&);
void setPluginProgramInStep (Song&, DeviceID, int program);

// A plug-in's value set by hand by a control, a command or a preset, in the caller's step (model/devices/Parameters.cpp:
// setValueSetByHand()): the song's value of the parameter set to it; where the song sets none, what the plug-in holds (held)
// written first, outside the history, as a held value (DeviceStoreValue.h: PluginValueEntry, never the song's), so that the
// step's Undo comes back to it and gives the plug-in that back. And the song's value of a parameter taken away in the caller's
// step (one set back to the parameter's default), leaving the value it had as a held value, so that a Redo of the step gives
// the plug-in that value. Nothing for a device the song hasn't, or that isn't a plug-in.
void setPluginValueSetByHand (Song&, DeviceID, const juce::String& parameter, float value, float held);
void removePluginValueSetByHand (Song&, DeviceID, const juce::String& parameter);

//==============================================================================
// A chain's mute and solo (model/mixer/internal/Containers.h: setChainMuted(), setChainSoloed()): each set without an undo
// step, as a track's mute and solo are, true or none; and both switched off in the caller's step (a chain left alone in its
// container, removeChain(), so that undoing it brings them back with the chain taken away). Nothing for a chain the song
// hasn't.
enum class ChainFlag
{
    mute,
    solo
};

bool isChainMuted (const Song&, DeviceID chain);
bool isChainSoloed (const Song&, DeviceID chain);
void setChainSwitch (Song&, DeviceID chain, ChainFlag, bool on);
void clearChainSwitchesInStep (Song&, DeviceID chain);

//==============================================================================
// What a copy of devices takes along of the store (the devices' writers: model/mixer/internal/Mixer.h, copyDevices();
// model/mixer/internal/Containers.h, duplicateChain()): the store's entries of the items copied (by their IDs before, as copied
// names them, the song's alone), under their copies' IDs (copied: each item's ID before, its copy's after); and each item's
// window's place given its copy, as view state outside the history. And the store's entries of what a maker made (by the made
// IDs), made the song's (the song's own values of each, a held one aside), which the devices' setter of new devices sets before
// it puts them in (Mixer.h: setNewDevices()), so that a device comes in with its state: outside the history for an item the
// store holds no entry of (a new ID), so that what is written for it later outside the history survives the Undo and Redo of
// the step that made it; in the caller's step for one it holds entries of (a device come back under an ID the store knows).
DeviceStore copyStoreOf (const Song&, const std::map<std::uint64_t, std::uint64_t>& copied);
void copyWindowPlaces (Song&, const std::map<std::uint64_t, std::uint64_t>& copied);
void setMadeStore (Song&, const std::vector<DeviceID>& made, const DeviceStore&);

//==============================================================================
// What the versions take along of the store (model/versions/ApplySong.cpp: makeDeviceStoreLike(), a version's devices made
// the song's; model/mixer/DeviceClashes.cpp: a merge's clash on a plug-in's state, program, format ID or version, on one of
// its values set by hand, or on a chain's mute or solo):
//   - getStoreOf(): what the store holds of one item (the song's entries alone), none for one the song hasn't.
//   - setStoreEntries(): what the store holds of one item made what entries hold of it (DeviceStoreValue.h: withEntriesOf(),
//     its own entries alone looked at, those of other items left out; a held value kept where the entries set none of its
//     parameter), in the mode of the moment (in the step in progress, or outside the history while the song is written
//     outside it: Song::OutsideHistory), nothing where it holds them so already. Nothing for an item that is neither a plug-in
//     nor a chain the song holds. Whether it changed the song's.
//   - forgetWindowPlace(): a device's window's place taken away, as view state outside the history (a version without one made
//     the song's).
DeviceStore getStoreOf (const Song&, DeviceID);
bool setStoreEntries (Song&, DeviceID, const DeviceStore& entries);
void forgetWindowPlace (Song&, DeviceID);

// How many times setStoreEntries() wrote a plug-in's values set by hand (once a call that changes any of them, however many:
// the store writes them as one map, sorted as a file holds them), by every song since the program started. For the tests,
// which check that a version that holds a plug-in's values as the song does writes none of them. Any thread.
std::uint64_t countPluginValuePlacesBuilt() noexcept;

//==============================================================================
// Where a device's window was, for the next time it opens, none for one never moved; and set as it moves, as view state:
// without an undo step, counted as no change (Schema.h: deviceWindowX, deviceWindowY), only where it differs. Nothing for a
// device the song hasn't.
std::optional<WindowPlace> getWindowPlace (const Song&, DeviceID);
void setWindowPlace (Song&, DeviceID, WindowPlace);
} // namespace phazite
