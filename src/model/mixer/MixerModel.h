// SPDX-License-Identifier: AGPL-3.0-or-later
// The mixer as the UI shows it (docs/design.md, section 8), by IDs and plain values: its channels in their order, what
// each is and holds and where its sound goes, and the handles its controls change things through (architecture phase
// A3: no Tracktion here). Each function takes the open project's context and gives nothing, or does nothing, for an ID
// of something that is gone. The changes are undo steps of their own, named as the functions in Mixer.h name them,
// unless they say otherwise.
#pragma once

#include "DeviceType.h"
#include "LevelSource.h"
#include "MixHandle.h"
#include "model/base/IDs.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/ParameterHandle.h"

#include <juce_graphics/juce_graphics.h>

#include <optional>
#include <vector>

namespace phazite
{
class ProjectContext;

//==============================================================================
// The channels: the tracks in the playlist's order, folders before the tracks in them; and the return tracks, which the
// mixer shows together next to the master.
struct MixerChannels
{
    std::vector<TrackID> tracks, returns;
};

MixerChannels getMixerChannels (const ProjectContext&);

enum class ChannelKind
{
    track,       // a track: an instrument, effects, sends
    bus,         // a folder whose tracks' sound mixes through its own effects, fader and meter
    plainFolder, // a folder that only scales its tracks' volumes, with no sound of its own
    returnTrack  // takes in what the sends to it feed it
};

struct Channel
{
    TrackID id;
    juce::String name;
    juce::Colour colour;
    ChannelKind kind = ChannelKind::track;
    bool hasEffects = false;      // a bus with effects stays one
    bool needsInstrument = false; // it has notes but no instrument to play them
    juce::StringArray senders;    // a return's: the tracks that send to it
    juce::String outputBus;       // the name of the folder bus its sound goes into; empty for the master
};

std::optional<Channel> getChannel (const ProjectContext&, TrackID);

// Whether one channel's sound reaches another's: through a send to a return, or by being in a bus folder.
bool feeds (const ProjectContext&, TrackID from, TrackID to);

// The channel the inspector shows: the selected track, or with none the track live MIDI plays, or else the first.
TrackID getInspectedChannel (const ProjectContext&);

//==============================================================================
// A volume's value as its fader moves it, a position from 0 to 1, and the level in decibels that it stands for.
float faderPositionToDecibels (float position);
float decibelsToFaderPosition (float decibels);

// A channel's controls. A plain folder's volume scales its tracks' volumes; it has no pan or level of its own.
ParameterHandle getVolume (ProjectContext&, TrackID);
ParameterHandle getPan (ProjectContext&, TrackID);
LevelSource getLevel (ProjectContext&, TrackID);

ParameterHandle getMasterVolume (ProjectContext&);
LevelSource getMasterLevel (ProjectContext&); // what the master plays, after its fader

struct MuteSolo
{
    bool muted = false, soloed = false;
    bool silencedBySolo = false; // other tracks' solos keep it quiet
};

std::optional<MuteSolo> getMuteSolo (const ProjectContext&, TrackID); // nothing for a track that is gone

// Not undo steps, as in many DAWs (model/arrangement/internal/Tracks.h).
void toggleMute (ProjectContext&, TrackID);
void toggleSolo (ProjectContext&, TrackID);      // along with the tracks soloed already
void toggleSoloAlone (ProjectContext&, TrackID); // alone, or unsolo it

void setMixedAsBus (ProjectContext&, TrackID, bool); // a folder's bus switch

//==============================================================================
// Devices, in slots: a track's instrument, then its effects; the master's effects (with an empty ID).
struct DeviceSlot
{
    DeviceID id;
    juce::String name;
    bool isOn = true;
    bool hasMix = false;         // an effect's mix (getMix())
    double latencySeconds = 0.0; // how late its sound comes out
    bool isContainer = false;    // a container of effects (ContainerModel.h)

    // A plug-in, or one of Tracktion's built-in devices: its own controls are in a window of its own (showDeviceWindow()).
    // Phazite's own devices and containers have none: the Devices view shows them whole (docs/design.md, section 9).
    bool opensWindow = false;
};

std::optional<DeviceSlot> getInstrument (const ProjectContext&, TrackID);
std::vector<DeviceSlot> getEffects (const ProjectContext&, TrackID);
bool canHoldInstrument (const ProjectContext&, TrackID); // a track that isn't a return

// As a slot's small knobs: a device's key parameters, or a container's first macros.
std::vector<ParameterHandle> getKeyControls (ProjectContext&, DeviceID, int maximum);
MixHandle getMix (ProjectContext&, DeviceID);

// These reach devices in containers too (ContainerModel.h), and move one among its container's devices.
void setDeviceOn (ProjectContext&, DeviceID, bool);
void moveEffect (ProjectContext&, DeviceID, int places); // earlier (negative) or later among its channel's effects
void removeDevice (ProjectContext&, DeviceID);
void showDeviceWindow (ProjectContext&, DeviceID); // a device that opens one (DeviceSlot::opensWindow); nothing for others

// One of Phazite's own devices, for its editor (model/devices/DeviceHandle.h); a handle of nothing for another device.
DeviceHandle getDeviceHandle (ProjectContext&, DeviceID);

// The channel a device is on, the devices in its containers included: a track, or an empty ID for the master; nothing
// for a device that is gone.
std::optional<TrackID> getDeviceChannel (const ProjectContext&, DeviceID);

// What can be added, and adding it (Mixer.h): an instrument for a track, in place of the one it had; an effect after a
// channel's effects (the master's, with an empty ID); a return holding an effect, with a send to it; and a device
// dropped from the browser, where addDeviceTo() puts it. The last two give what they added: the return, or whether
// anything was.
std::vector<DeviceType> getDeviceTypes (const ProjectContext&);
void chooseInstrument (ProjectContext&, TrackID, const DeviceType&);
void addEffect (ProjectContext&, TrackID, const DeviceType&);
TrackID addReturnWith (ProjectContext&, TrackID sender, const DeviceType&);
bool addDeviceTo (ProjectContext&, TrackID, const DeviceType&);

//==============================================================================
// Sends from a track, and the returns they go to.
struct Send
{
    DeviceID id;
    juce::String returnName;
    bool preFader = false;
};

std::vector<Send> getSends (const ProjectContext&, TrackID);
ParameterHandle getSendLevel (ProjectContext&, DeviceID);
void setPreFader (ProjectContext&, DeviceID, bool);
void removeSend (ProjectContext&, DeviceID);

struct Return
{
    TrackID id;
    juce::String name;
};

std::vector<Return> getReturns (const ProjectContext&);
void addSendTo (ProjectContext&, TrackID from, TrackID returnTrack);
void addSendToNewReturn (ProjectContext&, TrackID from);

//==============================================================================
// The master's loudness as streaming services measure it (EBU R 128), each reading until there is enough to measure.
struct Loudness
{
    std::optional<float> momentary, shortTerm, integrated; // LUFS
    std::optional<float> truePeak;                         // dBTP
};

std::optional<Loudness> getLoudness (const ProjectContext&); // nothing without a project, or a loudness meter on its master
void resetLoudness (ProjectContext&);
} // namespace phazite
