// SPDX-License-Identifier: AGPL-3.0-or-later
// The mixer's model (docs/design.md, section 8): a channel for every track, in the playlist's order; devices on
// tracks, folders and the master (folders are buses, Buses.h); return tracks, and sends to them from any track,
// before or after its fader.
#pragma once

#include "model/base/internal/Selection.h"
#include "model/base/internal/Tracktion.h"
#include "model/mixer/DeviceType.h"
#include "model/mixer/internal/Buses.h"

#include <memory>
#include <optional>
#include <vector>

namespace phazite
{
class CommandRegistry;
class Gesture;
struct DeviceSlot;

//==============================================================================
// Devices: the plug-ins a channel shows in its slots. A channel's volume, meter and sends are controls of its
// own, a return track's input shows in its header, and the master's loudness meter is the mixer's, so none of
// them is a device.
bool isDevice (const te::Plugin&);
juce::Array<te::Plugin*> getDevices (const te::Track&);
juce::Array<te::Plugin*> getMasterDevices (const te::Edit&);

// Whether a device has a window of its own (app/PluginWindow.h): a plug-in; not one of Phazite's own devices, nor a
// container, which the Devices view shows.
bool opensWindow (const te::Plugin&);

// Whether one of Phazite's own devices (not a container) can show in a window of its own when the user wants it, out of
// the Devices view; and whether a device's window shows now.
bool popsOut (const te::Plugin&);
bool isWindowOpen (const te::Plugin&);

// A track's instrument, which turns its notes into sound: its first device, if that is an instrument. Its effects
// are the devices after it, which change the sound. Folders, returns and the master have effects only.
te::Plugin* getInstrument (const te::Track&);
juce::Array<te::Plugin*> getEffects (const te::Track&);

// True if a track has notes but no instrument to play them, so they make no sound.
bool needsInstrument (const te::Track&);

// Puts an instrument on a track, in place of the one it had. Returns it, or nullptr if the track can't hold one.
te::Plugin* setInstrument (te::AudioTrack&, const te::Plugin::Ptr&);

// Adds a device to a track's chain, at a place among its devices (-1 for the end), before its fader. A plain
// folder becomes a bus first. Returns the device, or nullptr if the track can't hold it.
te::Plugin* insertDevice (te::Track&, const te::Plugin::Ptr&, int index = -1);

// Adds a device to the master, before its loudness meter.
te::Plugin* insertMasterDevice (te::Edit&, const te::Plugin::Ptr&, int index = -1);

// Removes a device, as one undo step named after it: a container with what it holds (Containers.h), and a device in a
// container from its row. A bus left without devices becomes a plain folder again.
void removeDevice (te::Plugin&);

// Moves a device to another place among its channel's devices, as one undo step.
void moveDevice (te::Plugin&, int newIndex);

// Turns a device on or off, as one undo step; and whether it is on.
void setDeviceOn (te::Plugin&, bool on);
bool isDeviceOn (const te::Plugin&);

// The song's devices (Schema.h: DEVICE, CONTAINER; T1 step 5), which Tracktion's devices mirror
// (model/mirror/internal/Mirror.h: reconcileDevices()): the element that holds a track's chain (its TRACK), or the
// master's (MASTER) for nullptr, invalid for an Edit that isn't a song's; a chain's devices' elements, in order (or a
// container's, given its element); a device's element, on a chain or in a container, invalid for one that isn't the
// song's; the ID an element names; whether it is a plug-in's; and the element of a device that the model has just made,
// with what its state holds (a plug-in's settings, as a preset made it; a container's devices).
juce::ValueTree findChainState (const te::Edit&, const te::Track*);
std::vector<juce::ValueTree> getDeviceStates (const juce::ValueTree& chain);
juce::ValueTree findDeviceState (const te::Edit&, DeviceID);
DeviceID getDeviceID (const juce::ValueTree& deviceState);
bool isPluginState (const juce::ValueTree& deviceState);
juce::ValueTree makeDeviceState (te::Plugin&);

// Which plug-in a device is, on its element in the song (Schema.h: pluginFormat to pluginUid), as its description says,
// written outside the undo history where it differs. The plug-in scanning found that a description names: by its
// identifier and name, else by its name, maker and format, as a preset finds it. And the description of the plug-in an
// element names: the one scanning found, or else one made of what the element says, which Tracktion keeps as a plug-in it
// can't load.
void setPluginIdentity (juce::ValueTree& device, const juce::PluginDescription&);
std::optional<juce::PluginDescription> findKnownPlugin (te::Engine&, const juce::PluginDescription&);
juce::PluginDescription getPluginDescription (te::Engine&, const juce::ValueTree& device);

// After a track was copied (a folder with its tracks), the devices' elements in the copy of its element name the copy's
// devices, which are where the original's are in its list, and its containers' devices their copies, as copyContainers()
// (Containers.h) mapped them.
void giveCopiedDeviceIDs (juce::ValueTree& copiedChain, const te::Track& copy, const te::EditItemID::IDMap& inContainers);

// Sets a device's parameter to a value set by hand, as it snaps to its range and steps: the song's, for one of Phazite's own
// devices in a chain (Schema.h: VALUES), and Tracktion's parameter, which plays it. And one of a device's files or tables
// (Schema.h: SOUNDS; empty for none), the song's and its PLUGIN's. In the caller's undo step. A channel's fader's volume and
// pan are the song's too (setChannelVolume()). A plug-in's parameter is the plug-in's own, which its state holds (stored as
// the project saves: DeviceStates.h); a send's, a modulator's or a macro's are Tracktion's alone while T1 lasts.
void setDeviceValue (te::AutomatableParameter&, float value);
void setDeviceSound (te::Plugin&, const juce::Identifier& property, const juce::String& value);

// Where in a channel's plug-ins a device goes, before the device at a place among its devices, or else after the last
// device (or after a return's input), so before the sends, the fader and the meters.
int getDevicePlace (const te::PluginList&, int deviceIndex);

// Where a device's own window was last (docs/design.md, section 9): view state, written without an undo step, which the
// project saves, so that the window opens there again. A save stores it too (model/devices/internal/DeviceStates.h),
// and finds it written already.
void rememberWindowPosition (te::Plugin&, juce::Point<int> topLeft);

// A plug-in's key parameters, by ID (MixerModel.h): its quick control, then the first others (not the dry and wet levels,
// which its mix sets). None for Phazite's own devices or a container.
juce::StringArray getKeyControls (te::Plugin&, int maximum);

// A device as a slot shows it (MixerModel.h).
DeviceSlot makeDeviceSlot (te::Plugin&);

// An effect's mix: how much of what it puts out is its effect, from 0 (only the sound that comes into it, "dry") to 1
// (only the effect, "wet"), as FL Studio's mixer slots and REAPER's effect chains have for every effect. It crossfades
// the dry and wet levels Tracktion gives each plug-in (and its reverb), linearly; a plug-in's is the song's (Schema.h:
// deviceMix). A device without them, such as an instrument, has no mix.
bool hasMix (te::Plugin&);
float getMix (te::Plugin&); // 1 for a device without a mix
void setMix (te::Plugin&, float mix);
te::AutomatableParameter::Array getMixParameters (te::Plugin&); // the dry and wet levels, or none

//==============================================================================
// The devices that can be added (DeviceType.h): Phazite's own and the plug-ins found by scanning.
std::vector<DeviceType> getDeviceTypes (te::Engine&);
te::Plugin::Ptr createDevice (te::Edit&, const DeviceType&);

// Adds a device dropped from the browser or chosen in the plug-in picker, as one named undo step. An instrument becomes a
// track's instrument, in place of the one it had; a track that can't hold one (a folder or a return), or none, gets a
// new track after it, named and colored after the instrument, and so does a track with audio clips, which an
// instrument would silence. An effect goes after the track's effects, or with no track on the master. Returns the
// device, or nullptr.
te::Plugin* addDeviceTo (te::Edit&, te::Track*, const DeviceType&, Selection*);

// The same for a device made already (from a preset), joining the caller's step.
te::Plugin* placeDevice (te::Edit&, te::Track*, const te::Plugin::Ptr&, Selection*);

// The device menus' choices (ui/devices/DeviceMenus.h), each one undo step named after the device: an instrument for a
// track, in place of the one it had ("Add Synth", or "Replace Instrument with Synth"); an effect after a track's
// effects, or with no track on the master ("Add Reverb"); and a return track holding an effect, with a send to it from
// a track (addReturnWithEffect(): "Add Reverb Return"). Each returns what it added, or nullptr.
te::Plugin* chooseInstrument (te::AudioTrack&, const DeviceType&);
te::Plugin* addEffect (te::Edit&, te::Track*, const DeviceType&);
te::AudioTrack* addReturnWith (te::Track& sender, const DeviceType&);

//==============================================================================
// Folders (Buses.h). The bus switch on a folder's strip, as an undo step. A folder made a bus with it, as every new
// folder is, stays one when its last effect goes, while a plain folder that became a bus by getting an effect (in a
// project made before folders started as buses) turns plain again then. A bus with effects stays a bus.
void setMixedAsBus (te::FolderTrack&, bool);

// The plug-in whose volume a channel's fader moves: a track's or a bus's volume, or a plain folder's VCA.
te::Plugin* getVolumeDevice (te::Track&);
juce::String getVolumeParameterID (te::Track&);
te::LevelMeasurer* getLevelMeasurer (te::Track&); // nullptr for a plain folder, which has no sound of its own

// A channel's volume, in decibels, and its pan, from -1 to 1 (Schema.h: channelVolume, channelPan; T1 step 5e): the
// song's, which its fader plays (a plain folder's VCA has no pan); the master track's are the master's, whose volume is
// -3 dB until it is set, as a new song's. Set in the caller's undo step; setDeviceValue() sets them too, given the fader's
// parameters.
inline constexpr float defaultMasterVolume = -3.0f;
void setChannelVolume (te::Track&, float decibels);
void setChannelPan (te::Track&, float pan);
float getChannelVolume (const te::Track&);
float getChannelPan (const te::Track&);

// The bus a channel's sound goes into: the nearest folder it is in that is a bus; nullptr for the master.
te::FolderTrack* getOutputBus (const te::Track&);

// The tracks the mixer shows as channels, in its order: the playlist's, then the returns.
juce::Array<te::Track*> getChannelTracks (const te::Edit&);

//==============================================================================
// Returns and sends. A return track takes in an aux bus (Tracktion's AuxReturnPlugin, its first plug-in), so its
// devices, such as a reverb, process what the sends feed it.
int getReturnBus (const te::Track&); // -1 if it isn't a return track
juce::Array<te::AudioTrack*> getReturnTracks (const te::Edit&);
te::AudioTrack* findReturnTrack (const te::Edit&, int bus);

// Adds a return track after the last track, on a bus of its own, named as given, and selects it.
te::AudioTrack* addReturnTrack (te::Edit&, const juce::String& name, Selection*);

// The same, named "Return" and its number, after the returns there are.
te::AudioTrack* addNewReturnTrack (te::Edit&, Selection*);

// In one step, as Studio One does: a return track holding an effect, named after it, and a send to it from a track,
// after the track's fader. The effect plays only its effect, at a mix of 100 % (Tracktion's reverb without its dry level):
// the dry sound reaches the master through the track itself.
// Returns the return track, or nullptr if there was no room for it.
te::AudioTrack* addReturnWithEffect (te::Track& sender, const te::Plugin::Ptr& effect);

juce::Array<te::AuxSendPlugin*> getSends (const te::Track&);

// Adds a send from a track to a return's bus: after the fader, or before it with preFader. Its level starts at
// -6 dB. Returns nullptr if the track can't hold it.
te::AuxSendPlugin* addSend (te::Track&, int bus, bool preFader);
bool isPreFader (const te::AuxSendPlugin&);
void setPreFader (te::AuxSendPlugin&, bool); // as one undo step, "Change Send"

// The send menus' choices, each one undo step named "Add Send": a send after the fader to a return's bus, or to a new,
// empty return. Each returns the send, or nullptr. And removing one, "Remove Send".
te::AuxSendPlugin* addSendTo (te::Track&, int bus);
te::AuxSendPlugin* addSendToNewReturn (te::Track&);
void removeSend (te::AuxSendPlugin&);

// The mixer's commands: add a return track.
void addMixerCommands (CommandRegistry&, Selection&);
} // namespace phazite
