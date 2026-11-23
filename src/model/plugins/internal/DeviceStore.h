// SPDX-License-Identifier: AGPL-3.0-or-later
// What the song stores of its devices in its undo history and outside it alike, read and written by IDs (roadmap step S5.20,
// the devices' area; the store's value: model/base/internal/DeviceStoreValue.h): a plug-in's state, program, format ID and
// version (Schema.h: pluginState, pluginProgram, pluginFormatID, pluginVersion) and its values set by hand (its VALUE
// children); a chain's mute and solo (chainMute, chainSolo); and, outside the history alone, a device's window place
// (deviceWindowX, deviceWindowY; model/base/internal/DevicesOutside.h). The one block of the model that reads and writes
// them in a live song: the plug-ins' instances (PluginInstances.h), the parameters' code (model/devices/Parameters.cpp), the
// containers' (model/mixer/Containers.cpp), a preset loaded into a plug-in (model/presets/Presets.cpp), the versions
// (model/versions/ApplySong.cpp, a merge's choice: model/mixer/DeviceClashes.cpp) and the plug-in's window
// (src/app/PluginWindow.cpp) reach them here alone.
//
// While the song's tree is their storage, each does on the tree exactly what its callers did on it: the same property of
// the same element, with the same undo manager or none, writing only what differs where they did (JUCE workaround W8), so
// that the tree's listeners hear the same callbacks in the same order. The store's switch changes what is inside these
// functions, to the song's keyed store and its value outside the history, and little else: what still takes an element is
// named so below. What plays the song reads a chain's mute and solo from the element it has in hand as it compiles a plan
// (DevicesValue.h: readChainMute(), readChainSolo()), which the switch points at the store too; and the tree's callbacks
// of what hears devices change (PluginInstances' own, what plays the song, the project) still hear these properties on the
// tree. Message thread only.
//
// By ID, on the tree, is a walk of the song (Mixer.h: findDeviceState(), countDeviceWalks()). So each function has a form
// that takes the element its caller holds already (a control's found parameter, a tree callback's element, one found once
// for several reads), which reads and writes it without a walk, as the callers did before the store; the form by ID finds
// the element once and calls it, for callers that hold none. No path walks the song more often than before the store
// (tests/PerformanceGateTests.cpp).
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/DeviceStoreValue.h"
#include "model/base/internal/DevicesOutside.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
class Song;

//==============================================================================
// A plug-in's state (its settings' bytes), empty for none and for a state that isn't base 64; its program, none for none, as
// the song holds it (an int, which a damaged file may make negative: a caller keeps it as it took it); its format ID and its
// version, none for none. Each nothing for a device the song hasn't, or that isn't a plug-in.
juce::MemoryBlock getPluginState (const Song&, DeviceID);
std::optional<int> getPluginProgram (const Song&, DeviceID);
juce::String getPluginFormatID (const Song&, DeviceID);
juce::String getPluginVersion (const Song&, DeviceID);

// The same of a plug-in's element the caller holds (none for an invalid one): the caller's element until the store's
// switch; by ID from then.
juce::MemoryBlock getPluginState (const juce::ValueTree& device);
std::optional<int> getPluginProgram (const juce::ValueTree& device);

// A plug-in's parameter, by the plug-in's own ID for it (a VST3's number), with a value of it.
struct PluginValueSet
{
    juce::String parameter {};
    float value = 0.0f;
};

// A plug-in's values set by hand, each parameter with its value, in the order the song holds them; and one parameter's, none
// for one the song sets none of (the first of two of one parameter, which the loader leaves none of).
std::vector<PluginValueSet> getPluginValues (const Song&, DeviceID);
std::optional<float> findPluginValue (const Song&, DeviceID, const juce::String& parameter);

// The same of a plug-in's element the caller holds (a control's found parameter's: model/devices/Parameters.cpp): the
// caller's element until the store's switch; by ID from then.
std::vector<PluginValueSet> getPluginValues (const juce::ValueTree& device);
std::optional<float> findPluginValue (const juce::ValueTree& device, const juce::String& parameter);

// One VALUE of a plug-in's, the element a tree callback names (PluginInstances: takeValueFromSong()), with its parameter and
// its value: the caller's element until the store's switch, whose listener names the store's entry from then.
PluginValueSet readPluginValue (const juce::ValueTree& value);

//==============================================================================
// What a plug-in's instance stores of it into the song, outside the undo history (PluginInstances: store(), its answers),
// each only where it differs from what the song holds (JUCE workaround W8 (docs/juce-workarounds.md)): its state, set where
// it holds bytes and taken away where it is empty (none: left as it is); its format ID and its version, each set where it
// isn't empty; its program, set where it is 0 or more and taken away at -1 (one of one program or none; none: left); and the
// values set by hand of the parameters automation or modulation moves, each given to the song's value of its parameter, one
// made where the song has none. Its state is the caller's block, which it keeps for the call's length alone (a plug-in's
// state can be megabytes: never copied for a store).
struct StoredPlugin
{
    const juce::MemoryBlock* state = nullptr;
    juce::String formatID {};
    juce::String version {};
    std::optional<int> program {};
    std::vector<PluginValueSet> values {};
};

void storePluginOutsideHistory (Song&, DeviceID, const StoredPlugin&);

// The same into the song's element of the plug-in the caller holds (PluginInstances: store() found it): the caller's
// element until the store's switch; by ID from then.
void storePluginOutsideHistory (juce::ValueTree device, const StoredPlugin&);

// The same into a plug-in's element held for an answer of its instance's still to come (PluginInstances: Answering), which a
// step may have taken out of the song meanwhile, where its Undo brings it back from: the one writer that takes an element,
// until the store's switch, which writes such an answer by the device's ID into the store, where the Undo finds it.
void storePluginIntoHeld (juce::ValueTree device, const StoredPlugin&);

// A plug-in's value set in its own window, outside the history: given to each value of the parameter the song holds, none
// made (PluginInstances: listen()).
void setPluginValueOutsideHistory (Song&, DeviceID, const juce::String& parameter, float value);

// A plug-in's state and program given by the song in the caller's undo step, as a preset loaded into it gives them
// (model/presets/Presets.cpp: loadPresetInto()): its state in base 64, taken away where it is empty (as text, as the preset
// holds it, or as bytes); its program. And the same into a plug-in's element the caller holds (loadPresetInto() found it):
// the caller's element until the store's switch; by ID from then.
void setPluginStateInStep (Song&, DeviceID, const juce::String& base64);
void setPluginStateInStep (Song&, DeviceID, const juce::MemoryBlock&);
void setPluginProgramInStep (Song&, DeviceID, int program);
void setPluginStateInStep (Song&, juce::ValueTree device, const juce::String& base64);
void setPluginProgramInStep (Song&, juce::ValueTree device, int program);

// A plug-in's value set by hand by a control, a command or a preset, in the caller's step (model/devices/Parameters.cpp:
// setValueSetByHand()): the song's value of the parameter set to it; where the song sets none, one made holding what the
// plug-in holds (held) and then set, so that an Undo gives the plug-in that back. And the song's value of a parameter taken
// away in the caller's step (one set back to the parameter's default). Each by ID, and into a plug-in's element the caller
// holds (a control's found parameter's): the caller's element until the store's switch; by ID from then.
void setPluginValueSetByHand (Song&, DeviceID, const juce::String& parameter, float value, float held);
void removePluginValueSetByHand (Song&, DeviceID, const juce::String& parameter);
void setPluginValueSetByHand (Song&, juce::ValueTree device, const juce::String& parameter, float value, float held);
void removePluginValueSetByHand (Song&, juce::ValueTree device, const juce::String& parameter);

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

// The same of a chain's element the caller holds (model/mixer/Containers.cpp: isChainHeard(), which reads its container's
// chains from the elements it walks through): the caller's element until the store's switch; by ID from then.
bool isChainMuted (const juce::ValueTree& chain);
bool isChainSoloed (const juce::ValueTree& chain);

void setChainSwitch (Song&, DeviceID chain, ChainFlag, bool on);
void clearChainSwitchesInStep (Song&, DeviceID chain);

// The same into a chain's element the caller holds (removeChain(), which has its container's chains in hand): the caller's
// element until the store's switch; by ID from then.
void clearChainSwitchesInStep (Song&, juce::ValueTree chain);

//==============================================================================
// What a copy of devices takes along of the store (the devices' writers: model/mixer/internal/Mixer.h, copyDevices();
// model/mixer/internal/Containers.h, duplicateChain()): what the store holds of every device, container and chain on a
// channel (a track's by its ID, the master's by its or none), its containers' chains and their devices, and each one's
// window's place where it has one, read from the channel's element alone (no walk of the song); and of a store's entries,
// those of the items copied, under their copies' IDs (none of another item's). And the store's entries of an item written
// into its element made for the song and not in it yet (DeviceStoreValue.h: writeStoreInto()), which the devices' setter of
// new devices writes as one element with them (Mixer.h: setNewDevices()), so that a device comes in with its state in the
// one action that puts it in.
struct ChannelStore
{
    DeviceStore store {};
    std::vector<std::pair<DeviceID, WindowPlace>> windows {};
};

ChannelStore getChannelStore (const Song&, TrackID channel);
DeviceStore renameStore (const DeviceStore&, const std::map<std::uint64_t, std::uint64_t>& copied);
void writeStoreIntoMade (juce::ValueTree& made, const DeviceStore&, DeviceID);

//==============================================================================
// What the versions take along of the store (model/versions/ApplySong.cpp: makeDeviceStoreLike(), a version's devices made
// the song's; model/mixer/DeviceClashes.cpp: a merge's clash on a plug-in's state, program, format ID or version, on one of
// its values set by hand, or on a chain's mute or solo):
//   - getStoreOf(): what the store holds of one item, as the song holds it (DeviceStoreValue.h: readHeldStoreOf()), none for
//     one the song hasn't; and getPluginValueOrder(), a plug-in's values' parameters in the order the song holds them.
//   - setStoreEntries(): what the store holds of one item made what entries hold of it (its own entries alone are looked
//     at: those of other items are left out), in the mode of the moment (in the step in progress, or without an undo manager
//     while the song is written outside its history: Song::OutsideHistory), each written only where it differs from what the
//     song holds as held: a plug-in's state (in base 64), its program, its format ID and its version, each set or taken
//     away; its values set by hand, each the entries hold none of taken away, each other set where it differs or made where
//     the song has none, then all in the order given (the parameters as the other song holds them; one it doesn't name after
//     them, in their order), as the passes over elements made a DEVICE's VALUE children the other's, a VALUE's own value as
//     a float; a chain's mute and solo, each set while on and taken away while off. Until the store's switch the order is
//     its VALUE children's; from then the store writes a plug-in's values sorted (DeviceStoreValue.h: writeStoreInto()), and
//     the order given goes. Nothing for an item that is neither a plug-in nor a chain. Whether it changed the song.
//   - readWindowPlace(): a window's place an element the caller holds (another song's, a version's) has, none for none; and
//     forgetWindowPlace(), a device's taken away, as view state outside the history (a version without one made the song's).
DeviceStore getStoreOf (const Song&, DeviceID);
std::vector<juce::String> getPluginValueOrder (const Song&, DeviceID);
bool setStoreEntries (Song&, DeviceID, const DeviceStore& entries, const std::vector<juce::String>& valueOrder);
std::optional<WindowPlace> readWindowPlace (const juce::ValueTree& device);
void forgetWindowPlace (Song&, DeviceID);

// How many times setStoreEntries() listed where a plug-in's values stand among its element's children to put them in order,
// by every song since the program started: once a call, and again after a move only where something else stands among them.
// For the tests, which check that putting a plug-in's values in order costs no more than their count. Any thread.
std::uint64_t countPluginValuePlacesBuilt() noexcept;

//==============================================================================
// Where a device's window was, for the next time it opens, none for one never moved; and set as it moves, as view state:
// without an undo step, counted as no change (Schema.h: deviceWindowX, deviceWindowY), only where it differs (JUCE
// workaround W8 (docs/juce-workarounds.md)). Nothing for a device the song hasn't.
std::optional<WindowPlace> getWindowPlace (const Song&, DeviceID);
void setWindowPlace (Song&, DeviceID, WindowPlace);
} // namespace phazite
