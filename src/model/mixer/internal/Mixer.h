// SPDX-License-Identifier: AGPL-3.0-or-later
// The mixer's model (docs/design.md, section 8): a channel for every track, in the playlist's order; devices on
// tracks, folders and the master (folders are buses, Buses.h); return tracks, and sends to them from any track,
// before or after its fader.
#pragma once

#include "model/base/internal/Tracktion.h"
#include "model/mixer/DeviceType.h"
#include "model/mixer/internal/Buses.h"

#include <memory>
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

// Whether a device has a window of its own (app/PluginWindow.h): a plug-in, or one of Tracktion's built-in devices; not
// one of Phazite's own, nor a container, which the Devices view shows.
bool opensWindow (const te::Plugin&);

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

// Turns a device on or off, as one undo step.
void setDeviceOn (te::Plugin&, bool on);

// Where a device's own window was last (docs/design.md, section 9): view state, written without an undo step, which the
// project saves, so that the window opens there again. Tracktion writes it as it saves too, through the undo manager,
// but finds it written already.
void rememberWindowPosition (te::Plugin&, juce::Point<int> topLeft);

// The parameters a device's slot shows as small knobs, by ID: its quick control, then the first others (not the dry
// and wet levels, which its mix sets). A container has none of its own: its key controls are its macros.
juce::StringArray getKeyControls (te::Plugin&, int maximum);

// A device as a slot shows it (MixerModel.h).
DeviceSlot makeDeviceSlot (te::Plugin&);

// An effect's mix: how much of what it puts out is its effect, from 0 (only the sound that comes into it, "dry") to 1
// (only the effect, "wet"), as FL Studio's mixer slots and REAPER's effect chains have for every effect. It crossfades
// the dry and wet levels Tracktion gives each plug-in (and its reverb), linearly. A device without them, such as an
// instrument, has no mix.
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
te::Plugin* addDeviceTo (te::Edit&, te::Track*, const DeviceType&, te::SelectionManager*);

// The same for a device made already (from a preset), joining the caller's step.
te::Plugin* placeDevice (te::Edit&, te::Track*, const te::Plugin::Ptr&, te::SelectionManager*);

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
te::AudioTrack* addReturnTrack (te::Edit&, const juce::String& name, te::SelectionManager*);

// The same, named "Return" and its number, after the returns there are.
te::AudioTrack* addNewReturnTrack (te::Edit&, te::SelectionManager*);

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
void addMixerCommands (CommandRegistry&, te::SelectionManager&);
} // namespace phazite
