// SPDX-License-Identifier: AGPL-3.0-or-later
// The mixer's model (docs/design.md, section 8): a channel for every track, in the playlist's order; devices on
// tracks, folders and the master (folders are buses, Buses.h); return tracks, and sends to them from any track,
// before or after its fader. The song holds them all (Schema.h: DEVICE, CONTAINER, SEND, and each channel's fader on its
// TRACK or MASTER), by their IDs, and what plays them is made from the song.
//
// A channel is named by its track's ID; the master's is the master's ID (Tracks.h: getMasterID()), or none, as the
// mixer's public model names it (MixerModel.h).
#pragma once

#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/Selection.h"
#include "model/mixer/DeviceType.h"
#include "model/mixer/internal/Buses.h"

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace phazite
{
class CommandRegistry;
class PluginHost;
class ProjectContext;
struct DeviceSlot;

//==============================================================================
// The song's devices: the element that holds a channel's chain (its TRACK, or MASTER for the master or none); a chain's
// devices' elements, in order (or a container's, given its element); a device's element, on a chain or in a container,
// invalid for one the song hasn't; the ID an element names; and what it is.
juce::ValueTree findChainState (const Song&, TrackID channel);
std::vector<juce::ValueTree> getDeviceStates (const juce::ValueTree& chain);
juce::ValueTree findDeviceState (const Song&, DeviceID);
DeviceID getDeviceID (const juce::ValueTree& deviceState);
bool isPluginState (const juce::ValueTree& deviceState);
bool isContainerState (const juce::ValueTree& deviceState);

// Whether a device's element is an instrument's, which turns notes into sound: one of Phazite's own instruments, or a plug-in
// that is one (Schema.h: pluginInstrument).
bool isInstrumentState (const juce::ValueTree& deviceState);

// A channel's devices, in their order: a track's instrument, if it has one (its first device), then its effects, which
// change the sound; folders, returns and the master have effects only. And the channel a device is on, the devices in its
// containers included: a track's ID or the master's; none for a device the song hasn't.
std::vector<DeviceID> getDevices (const Song&, TrackID channel);
DeviceID getInstrument (const Song&, TrackID);
std::vector<DeviceID> getEffects (const Song&, TrackID channel);
bool hasInstrument (const Song&, TrackID);
TrackID getDeviceChannel (const Song&, DeviceID);

// True if a track has notes but no instrument to play them, so they make no sound.
bool needsInstrument (const Song&, TrackID);

// A device's name: one of Phazite's own devices' (its type's), a container's own, or a plug-in's; by its ID in the song, or
// from its element.
juce::String getDeviceName (const Song&, DeviceID);
juce::String getDeviceName (const juce::ValueTree& deviceState);

// Whether a device has a window of its own (app/PluginWindow.h): a plug-in; not one of Phazite's own devices, nor a
// container, which the Devices view shows. And whether one of Phazite's own devices (not a container) can show in a
// window of its own when the user wants it, out of the Devices view.
bool opensWindow (const Song&, DeviceID);
bool popsOut (const Song&, DeviceID);

//==============================================================================
// New devices' elements, with IDs of the song's: one of Phazite's own (by its type ID), with values set by hand by its
// parameters' IDs (Schema.h: VALUES); or a device of a type the browser and the menus offer (DeviceType.h): one of
// Phazite's own, or a plug-in by its description (Schema.h: pluginFormat to pluginInstrument).
juce::ValueTree makeOwnDeviceState (Song&, std::string_view typeID, const std::vector<std::pair<juce::String, float>>& values = {});
juce::ValueTree makeDeviceState (Song&, const DeviceType&);

// Puts a device's element on a channel, at a place among its devices (-1: after them), an instrument first; a plain folder
// becomes a bus first. What plays it is made from it. Joins the caller's step. Returns its ID, or none for a channel that
// can't hold it.
DeviceID insertDeviceState (Song&, TrackID channel, juce::ValueTree device, int index = -1);

// Puts an instrument on a track, in place of the one it had, in the caller's step. Returns it, or none if the track can't
// hold one (a folder, a return or the master).
DeviceID setInstrument (Song&, TrackID, juce::ValueTree instrument);

// Removes a device, as one undo step named after it: a container with what it holds (Containers.h), and a device in a
// container from its row, what it holds of its own stored first (Song::storeDeviceStates()), so that an undo brings it
// back as it was.
// A bus left without devices becomes a plain folder again, unless it was made a bus on purpose.
void removeDevice (Song&, DeviceID);

// Moves a device to another place among its channel's devices, as one undo step.
void moveDevice (Song&, DeviceID, int newIndex);

// Turns a device on or off, as one undo step; and whether it is on.
void setDeviceOn (Song&, DeviceID, bool on);
bool isDeviceOn (const Song&, DeviceID);

// One of a device's files or tables (Schema.h: SOUNDS; empty for none), in the caller's undo step.
void setDeviceSound (Song&, DeviceID, const juce::Identifier& property, const juce::String& value);

// Where a device's own window was last (docs/design.md, section 9): view state in the song's element of the device
// (Schema.h: deviceWindowX, deviceWindowY), written without an undo step, which the project saves, so that the window
// opens there again.
void rememberWindowPosition (Song&, DeviceID, juce::Point<int> topLeft);
std::optional<juce::Point<int>> getWindowPosition (const Song&, DeviceID);

// An effect's mix: how much of what it puts out is its effect, from 0 (only the sound that comes into it, "dry") to 1
// (only the effect, "wet"), as FL Studio's mixer slots and REAPER's effect chains have for every effect: one of Phazite's
// own effects' mix parameter, or a plug-in's or a container's mix (Schema.h: deviceMix). An instrument has none. Set in
// the caller's step.
bool hasMix (const Song&, DeviceID);
float getMix (const Song&, DeviceID); // 1 for a device without a mix
void setMix (Song&, DeviceID, float mix);

// Which plug-in a device is, on its element in the song (Schema.h: pluginFormat to pluginInstrument), as its description
// says, written outside the undo history where it differs; and the description of the plug-in an element names, as the
// element has it.
void setPluginIdentity (juce::ValueTree& device, const juce::PluginDescription&);
juce::PluginDescription getPluginIdentity (const juce::ValueTree& device);

//==============================================================================
// Adds a device dropped from the browser or chosen in the plug-in picker, as one named undo step. An instrument becomes a
// track's instrument, in place of the one it had; a track that can't hold one (a folder or a return), or none, gets a
// new track after it, named and colored after the instrument, and so does a track with audio clips, which an
// instrument would silence. An effect goes after the track's effects, or with no track on the master. Returns the
// device, or none.
DeviceID addDeviceTo (Song&, TrackID, const DeviceType&, Selection*);

// The same for a device's element made already (from a preset), joining the caller's step.
DeviceID placeDevice (Song&, TrackID, juce::ValueTree device, Selection*);

// The device menus' choices (ui/devices/DeviceMenus.h), each one undo step named after the device: an instrument for a
// track, in place of the one it had ("Add Synth", or "Replace Instrument with Synth"); an effect after a track's
// effects, or with no track on the master ("Add Reverb"); and a return track holding an effect, with a send to it from
// a track (addReturnWithEffect(): "Add Reverb Return"). Each returns what it added, or none.
DeviceID chooseInstrument (Song&, TrackID, const DeviceType&);
DeviceID addEffect (Song&, TrackID channel, const DeviceType&);
TrackID addReturnWith (Song&, TrackID sender, const DeviceType&);

//==============================================================================
// Folders (Buses.h). The bus switch on a folder's strip, as an undo step. A folder made a bus with it, as every new
// folder is, stays one when its last effect goes, while a plain folder that became a bus by getting an effect turns plain
// again then. A bus with effects stays a bus.
void setMixedAsBus (Song&, TrackID folder, bool);

// What sets a channel's volume (Schema.h: channelFader, or channelVCA for a plain folder), the master's for the master,
// and the IDs of its parameters, which automation and modulation name ("volume" and "pan", "master volume" and "master
// pan", or a VCA's "vca"). And the channel's element whose fader a device is, invalid for another device.
DeviceID getFader (const Song&, TrackID channel);
juce::String getVolumeParameterID (const Song&, TrackID channel);
juce::String getPanParameterID (const Song&, TrackID channel); // none for a plain folder's VCA
juce::ValueTree findChannelOfFader (const Song&, DeviceID fader);

// A channel's volume, in decibels, and its pan, from -1 to 1 (Schema.h: channelVolume, channelPan): the song's, which its
// fader plays (a plain folder's VCA has no pan); the master's are the master's, whose volume is -3 dB until it is set, as a
// new song's. Set in the caller's undo step.
inline constexpr float defaultMasterVolume = -3.0f;
void setChannelVolume (Song&, TrackID channel, float decibels);
void setChannelPan (Song&, TrackID channel, float pan);
float getChannelVolume (const Song&, TrackID channel);
float getChannelPan (const Song&, TrackID channel);

// The bus a channel's sound goes into: the nearest folder it is in that is a bus; none for the master.
TrackID getOutputBus (const Song&, TrackID);

// The tracks the mixer shows as channels, in its order: the playlist's, then the returns.
std::vector<TrackID> getChannelTracks (const Song&);

//==============================================================================
// Returns and sends. A return track takes in what the sends to it feed it, so its devices, such as a reverb, process it.
std::vector<TrackID> getReturnTracks (const Song&);

// Adds a return track after the last track, named as given, and selects it if a selection is given. Solo-safe, as Studio
// One's effect channels are: soloing a track keeps its reverb, say, audible. Joins the caller's step.
TrackID addReturnTrack (Song&, const juce::String& name, Selection*);

// The same, named "Return" and its number, after the returns there are.
TrackID addNewReturnTrack (Song&, Selection*);

// In one step, as Studio One does: a return track holding an effect, named after it, and a send to it from a track,
// after the track's fader. The effect plays only its effect, at a mix of 100 %: the dry sound reaches the master through
// the track itself. Returns the return track, or none.
TrackID addReturnWithEffect (Song&, TrackID sender, juce::ValueTree effect);

// A track's sends (Schema.h: SEND), each to a return by the return's track, in their order; a send's element (invalid
// for one the song hasn't) and where it goes; and adding one from a track to a return: after the fader, or before it with
// preFader, its level starting at -6 dB. Returns none if the track can't send there or the return isn't one. Joins the
// caller's step.
std::vector<DeviceID> getSends (const Song&, TrackID);
juce::ValueTree findSendState (const Song&, DeviceID send);
TrackID getSendTarget (const Song&, DeviceID send);
DeviceID addSend (Song&, TrackID from, TrackID returnTrack, bool preFader);
bool isPreFader (const Song&, DeviceID send);
void setPreFader (Song&, DeviceID send, bool); // as one undo step, "Change Send"

// A send's level in decibels, the song's, in the caller's step.
void setSendLevel (Song&, DeviceID send, float decibels);
float getSendLevel (const Song&, DeviceID send);

// The send menus' choices, each one undo step named "Add Send": a send after the fader to a return, or to a new, empty
// return. Each returns the send, or none. And removing one, "Remove Send".
DeviceID addSendTo (Song&, TrackID from, TrackID returnTrack);
DeviceID addSendToNewReturn (Song&, TrackID from);
void removeSend (Song&, DeviceID send);

// The mixer's commands: add a return track.
void addMixerCommands (CommandRegistry&, Selection&);

// A device as a slot of the open project's shows it (MixerModel.h): with how late its sound comes out, whether its plug-in
// crashed, whether it is frozen with its track (FreezeModel.h), and whether its window shows (the project context's
// DeviceWindowHost).
DeviceSlot makeDeviceSlot (const ProjectContext&, DeviceID);

//==============================================================================
// The devices that can be added (DeviceType.h): Phazite's own and the plug-ins found by scanning, which the plug-in host
// keeps (model/plugins/internal/PluginHost.h). The plug-in scanning found that a description names: by its identifier and
// name, else by its name, maker and format, as a preset finds it. And the description of the plug-in an element names:
// the one scanning found, or else the element's own, for a plug-in that isn't installed here.
std::vector<DeviceType> getDeviceTypes (const PluginHost&);
std::optional<juce::PluginDescription> findKnownPlugin (const PluginHost&, const juce::PluginDescription&);
juce::PluginDescription getPluginDescription (const PluginHost&, const juce::ValueTree& device);

// A plug-in's key parameters, by its own IDs (MixerModel.h): its first parameters that can be automated, as its instance
// tells them, up to so many. None for a plug-in that doesn't play (with no instance), for Phazite's own devices, whose
// editors show all of theirs, or for a container, which shows its macros.
juce::StringArray getKeyControls (const Song&, DeviceID, int maximum);
} // namespace phazite
