// SPDX-License-Identifier: AGPL-3.0-or-later
// The mixer's model (docs/design.md, section 8): a channel for every track, in the playlist's order; devices on
// tracks, folders and the master (a folder becomes a bus when it holds one); return tracks, and sends to them
// from any track, before or after its fader.
#pragma once

#include "model/base/Tracktion.h"

#include <vector>

namespace phazite
{
class CommandRegistry;

//==============================================================================
// Devices: the plug-ins a channel shows in its slots. A channel's volume, meter and sends are controls of its
// own, a return track's input shows in its header, and the master's loudness meter is the mixer's, so none of
// them is a device.
bool isDevice (const te::Plugin&);
juce::Array<te::Plugin*> getDevices (const te::Track&);
juce::Array<te::Plugin*> getMasterDevices (const te::Edit&);

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

// Removes a device. A bus left without devices becomes a plain folder again.
void removeDevice (te::Plugin&);

// Moves a device to another place among its channel's devices.
void moveDevice (te::Plugin&, int newIndex);

// The parameters a device's slot shows as small knobs, by ID: its quick control, then the first others (not the dry
// and wet levels, which its mix sets).
juce::StringArray getKeyControls (te::Plugin&, int maximum);

// An effect's mix: how much of what it puts out is its effect, from 0 (only the sound that comes into it, "dry") to 1
// (only the effect, "wet"), as FL Studio's mixer slots and REAPER's effect chains have for every effect. It crossfades
// the dry and wet levels Tracktion gives each plug-in (and its reverb), linearly. A device without them, such as an
// instrument, has no mix.
bool hasMix (te::Plugin&);
float getMix (te::Plugin&); // 1 for a device without a mix
void setMix (te::Plugin&, float mix);
te::AutomatableParameter::Array getMixParameters (te::Plugin&); // the dry and wet levels, or none

//==============================================================================
// The devices that can be added: Tracktion's built-in ones and the plug-ins found by scanning.
struct DeviceType
{
    juce::String name;
    juce::String category; // "Instrument" or "Effect"
    juce::PluginDescription description;
    bool isInstrument = false;
};

// A plug-in description's device: an instrument or an effect, named as the description names it.
DeviceType makeDeviceType (const juce::PluginDescription&);

std::vector<DeviceType> getDeviceTypes (te::Engine&);
te::Plugin::Ptr createDevice (te::Edit&, const DeviceType&);

// Adds a device dropped from the browser or chosen in the plug-in picker, as one named undo step. An instrument becomes a
// track's instrument, in place of the one it had; a track that can't hold one (a folder or a return), or none, gets a
// new track after it, named and colored after the instrument, and so does a track with audio clips, which an
// instrument would silence. An effect goes after the track's effects, or with no track on the master. Returns the
// device, or nullptr.
te::Plugin* addDeviceTo (te::Edit&, te::Track*, const DeviceType&, te::SelectionManager*);

//==============================================================================
// Folders. A plain folder's volume scales its tracks' volumes, as a VCA does; a bus mixes its tracks
// together, through its devices, its own fader and meter.
bool isBus (const te::Track&);
void makeBus (te::FolderTrack&);
void makePlainFolder (te::FolderTrack&);

// The bus switch on a folder's strip, as an undo step. A folder made a bus with it stays one when its last effect
// goes, while one that became a bus by getting an effect turns plain again then. A bus with effects stays a bus.
void setMixedAsBus (te::FolderTrack&, bool);

// The plug-in whose volume a channel's fader moves: a track's or a bus's volume, or a plain folder's VCA.
te::Plugin* getVolumeDevice (te::Track&);
juce::String getVolumeParameterID (te::Track&);
te::LevelMeasurer* getLevelMeasurer (te::Track&); // nullptr for a plain folder, which has no sound of its own

//==============================================================================
// Returns and sends. A return track takes in an aux bus (Tracktion's AuxReturnPlugin, its first plug-in), so its
// devices, such as a reverb, process what the sends feed it.
int getReturnBus (const te::Track&); // -1 if it isn't a return track
juce::Array<te::AudioTrack*> getReturnTracks (const te::Edit&);
te::AudioTrack* findReturnTrack (const te::Edit&, int bus);

// Adds a return track after the last track, on a bus of its own, named as given, and selects it.
te::AudioTrack* addReturnTrack (te::Edit&, const juce::String& name, te::SelectionManager*);

// In one step, as Studio One does: a return track holding an effect, named after it, and a send to it from a track,
// after the track's fader. A built-in reverb drops its dry sound, which reaches the master through the track itself.
// Returns the return track, or nullptr if there was no room for it.
te::AudioTrack* addReturnWithEffect (te::Track& sender, const te::Plugin::Ptr& effect);

juce::Array<te::AuxSendPlugin*> getSends (const te::Track&);

// Adds a send from a track to a return's bus: after the fader, or before it with preFader. Its level starts at
// -6 dB. Returns nullptr if the track can't hold it.
te::AuxSendPlugin* addSend (te::Track&, int bus, bool preFader);
bool isPreFader (const te::AuxSendPlugin&);
void setPreFader (te::AuxSendPlugin&, bool);

// The mixer's commands: add a return track.
void addMixerCommands (CommandRegistry&, te::SelectionManager&);
} // namespace phazite
