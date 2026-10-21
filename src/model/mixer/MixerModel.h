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
#include <utility>
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

    // A plug-in: its own controls are in a window of its own (showDeviceWindow()). Phazite's own devices show whole in the
    // Devices view, and in a window of their own too when the user wants (popsOut); containers only there.
    bool opensWindow = false;
    bool popsOut = false;
    bool windowOpen = false; // one of Phazite's own devices, in its window now (isDeviceWindowOpen() also tells a plug-in's)

    // Its sound is in its track's frozen file (FreezeModel.h): its controls are off, and its window shut, until the track
    // is unfrozen.
    bool frozen = false;

    // It can listen to another track (a sidechain): the one it does, if any, with its name and colour.
    bool hasSidechain = false;
    SidechainChoice sidechain {};
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
// A device's window (DeviceSlot::opensWindow, popsOut), shown, or brought to the front if it shows; and whether it shows
// (its close button puts it back into the Devices view). Nothing for a device without one.
void showDeviceWindow (ProjectContext&, DeviceID);
bool isDeviceWindowOpen (const ProjectContext&, DeviceID);

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

// A send knob for every return on every strip, off until first touched (docs/design.md, section 8; question 13): the
// returns a track can send to (not itself, nor one whose sound comes from it), each with its send if it has one.
struct SendTarget
{
    TrackID returnTrack;
    juce::String name;
    std::optional<Send> send; // none until the knob is first turned
};

std::vector<SendTarget> getSendTargets (const ProjectContext&, TrackID);

// The level of a track's send to a return, its knob's handle: the send is made, after the fader, by the first change,
// in that change's undo step, "Send to" and the return's name; later changes are "Change Send Level".
ParameterHandle getSendLevelTo (ProjectContext&, TrackID from, TrackID returnTrack);

//==============================================================================
// Where a channel's sound goes (question 12): to the master, or into a folder's bus, which a track gets into by going
// into the folder, as the playlist shows it. A return goes to the master.
struct OutputChoice
{
    TrackID bus; // a folder that is a bus, or none for the master
    juce::String name;
    bool chosen = false;
};

std::vector<OutputChoice> getOutputChoices (const ProjectContext&, TrackID); // nothing for a return
bool canChooseOutput (const ProjectContext&, TrackID);

// A step of its own, "Change Output": the channel goes to the end of the bus's folder, or for the master, out of its
// folders, after the outermost.
void setOutput (ProjectContext&, TrackID, TrackID bus);

//==============================================================================
// A device's sidechain (docs/design.md, section 8), Phazite's own or a plug-in's: whether it has one, every track but its
// own with why it can't listen to one whose sound comes from its own, the one it listens to (none: the sound passing
// through it), and choosing one, as one undo step (Phazite's compressor, still as it was made, starts ducking then:
// internal/Sidechain.h).
bool hasSidechain (const ProjectContext&, DeviceID);
std::vector<SidechainChoice> getSidechainChoices (const ProjectContext&, DeviceID);
TrackID getSidechainSource (const ProjectContext&, DeviceID);
void setSidechainSource (ProjectContext&, DeviceID, TrackID source);

// Ducking a track under another, as one undo step ("Duck Bass Under Kick"): Phazite's compressor after its effects, its
// sidechain listening to the other. The tracks it can duck under: every track but itself, with why not one whose sound
// comes from it. Returns the compressor.
std::vector<SidechainChoice> getDuckUnderChoices (const ProjectContext&, TrackID ducked);
DeviceID duckUnder (ProjectContext&, TrackID ducked, TrackID key);

//==============================================================================
// How much of each block's time a channel's devices take to process, from 0 to 1, as Tracktion measures each device (its
// containers' devices included): what a strip's CPU meter shows. Read it from a timer.
float getCpuLoad (const ProjectContext&, TrackID);

//==============================================================================
// The routing view's wires (docs/design.md, section 8): where each channel's sound goes (its bus, or the master), its
// sends to returns, and the sidechains that listen to it.
struct Connection
{
    enum class Kind
    {
        output,
        send,
        sidechain
    };

    Kind kind = Kind::output;
    TrackID from {};    // the channel whose sound it carries
    TrackID to {};      // a bus, a return, or the channel of the device that listens; none for the master
    DeviceID device {}; // a send, or the device whose sidechain listens

    bool operator== (const Connection&) const = default;
};

std::vector<Connection> getConnections (const ProjectContext&);

//==============================================================================
// The master's loudness as streaming services measure it (EBU R 128), each reading until there is enough to measure.
struct Loudness
{
    std::optional<float> momentary, shortTerm, integrated; // LUFS
    std::optional<float> truePeak;                         // dBTP
};

std::optional<Loudness> getLoudness (const ProjectContext&); // nothing without a project, or a loudness meter on its master
void resetLoudness (ProjectContext&);

//==============================================================================
// Delay compensation (docs/design.md, section 20): how long all of the song's sound waits so that every track's comes out
// with the latest device's, what is played live too; none while low latency is on, which turns it off. And the devices
// that look ahead, switched on, latest first, each named with its channel ("Limiter on Master") and its latency.
struct DelayCompensation
{
    double seconds = 0.0;
    std::vector<std::pair<juce::String, double>> devices;
};

DelayCompensation getDelayCompensation (const ProjectContext&);
} // namespace phazite
