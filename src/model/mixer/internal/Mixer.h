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
#include "model/base/internal/DeviceStoreValue.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/ModulationValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/Selection.h"
#include "model/base/internal/SendsValue.h"
#include "model/base/internal/TracksValue.h"
#include "model/mixer/DeviceType.h"
#include "model/mixer/internal/Buses.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
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
// The song's devices, containers and chains as values (model/base/internal/DevicesValue.h), the devices' area of the song's
// values (SongValues.h: devices), which every reader of a device reads them as (roadmap step S5.20): all of them, by
// reference, the area's value as it is now, valid until the area's next change (as getCurvesValue()); one by its ID (a
// device, a container or a chain; nullptr for one the song hasn't), a pointer into that value, the same; and whether the song
// holds one, whether one is a plug-in and whether one turns notes into sound (DeviceValue::isInstrument()). Each a look in
// the area by an ID, which allocates nothing (counted: countDeviceLooks()), never a walk of the song. Keep no pointer or
// reference across a writer (a set makes a new value), nor across a turn of the message loop. A song that holds no values
// (the loader's, as it opens; the tests' check of a song made of a tree) reads its tree: all of them read whole again
// (DevicesValue.h: readDevicesValue(), counted: countDevicesReadAsValues()), kept for the thread in place of what was read
// before unless equal to it, so that what reads the one tree several times holds what it found while the tree doesn't change;
// one by its ID, its one element found and read whole (counted the same), kept for the thread among the last 16 read: its
// pointer is good through the next 15 looks on that thread that find a device of a song without values, whatever the song.
// The loader's checks of the curves and the MIDI links read neither (Parameters.h: locateParameter() with the devices'
// fields given); its check of the sidechains' loops reads one for a pad's track (getDeviceChannel()).
const SongDevices& getDevicesValue (const Song&);
const DeviceValue* findDevice (const Song&, DeviceID);
bool hasDevice (const Song&, DeviceID);
bool isPlugin (const Song&, DeviceID);
bool isInstrument (const Song&, DeviceID);

// How many looks in the devices' area by an ID (findDevice(), and what reads through it) since Phazite started, on any
// thread: for the tests that a control's tick, a strip's slot and a setter find a device by as many looks however many tracks
// the song has (tests/PerformanceGateTests.cpp).
std::uint64_t countDeviceLooks() noexcept;

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
// from its element in a copy (a version's, compared: model/versions/SongDiff.cpp).
juce::String getDeviceName (const Song&, DeviceID);
juce::String getDeviceName (const juce::ValueTree& deviceInCopy);

// Whether a device has a window of its own (app/PluginWindow.h): a plug-in; not one of Phazite's own devices, nor a
// container, which the Devices view shows. And whether one of Phazite's own devices (not a container) can show in a
// window of its own when the user wants it, out of the Devices view.
bool opensWindow (const Song&, DeviceID);
bool popsOut (const Song&, DeviceID);

//==============================================================================
// What the readers outside the devices' own code read of the song's devices, by IDs and plain values, each a look in the
// devices' area (getDevicesValue()). Whether the song holds a device or a container by its ID is DevicesValue.h's
// holdsDevice() (a look in the area).
//
// A channel's devices and containers in their order, each container followed by its chains and each chain by its devices,
// in theirs: each by its ID with what it is, its channel (a track's ID, or the master's ID for the master's), the container
// and the chain it is in, Phazite's own device's type (deviceType), whether it turns notes into sound
// (DeviceValue::isInstrument()), a plug-in's identity and the track a device's sidechain listens to (as the song holds it,
// whether or not that track can be heard: Sidechain.h, getSidechainSource()). The channel's list and its owners' looked in,
// no walk of the song; none for a channel the song hasn't. A track's by its ID; the master's by its ID or by none
// (TrackID()), as getDevices() has always read the master's for none. And
// every channel's, in the song's order: each track's (a folder's before the tracks in it), then the master's. For the
// project's plug-ins, automation's lanes, notes' drum mode, modulation's owners, the freeze and the versions' plug-ins.
struct DeviceOutline
{
    DeviceID id {};
    DeviceValue::Form form = DeviceValue::Form::own;
    TrackID channel {};
    DeviceID container {}, chain {};
    juce::String type {};
    bool instrument = false;
    PluginIdentity plugin {};
    TrackID sidechain {};
};

std::vector<DeviceOutline> getDeviceOutline (const Song&, TrackID channel);
std::vector<DeviceOutline> getSongDeviceOutline (const Song&);

// The description of one of Phazite's own devices by its ID (nullptr for a plug-in, a container, a chain or one the song
// hasn't); and whether a track's devices, those in its containers too, hold a plug-in (the arrangement's selection, which
// gets the plug-ins' states before it copies the track).
const devices::DeviceInfo* getOwnDeviceInfo (const Song&, DeviceID);
bool holdsPlugin (const Song&, TrackID);

// Where a container is by its ID: a look in the area; none for anything but a container the song holds. For the place a
// modulator of a container's is kept at (model/modulation/Modulation.cpp: findOwnerPlace()).
std::optional<DevicePlace> findContainerPlace (const Song&, DeviceID container);

// The sound files Phazite's own devices name (Schema.h: SOUNDS, schema::isSoundFileProperty()), each by its device, its
// property and its path as the song keeps it: a channel's devices', in the outline's order (getDeviceOutline()), for the
// project's files; or every device's of the song, in its tree's order, for the paths the song plays (AudioClips.h:
// findAudioFilePaths(), what Clean Up and an import keep).
struct DeviceSoundFile
{
    DeviceID device {};
    juce::Identifier property {};
    juce::String path {};
};

std::vector<DeviceSoundFile> getDeviceSoundFiles (const Song&, TrackID channel);
std::vector<DeviceSoundFile> getDeviceSoundFiles (const Song&);

// A device's or a container's element whole (model/base/internal/WholeState.h: writeDeviceTree(), of the song's state): with
// what a plug-in keeps of its own as the song holds it (store it first: Song::storeDeviceStates()), its window's place, a
// container's chains with their devices and its macros. What a preset and the DAWproject export read a device from. An
// invalid tree for one the song hasn't (a chain is none of these).
juce::ValueTree copyWholeDevice (const Song&, DeviceID);

//==============================================================================
// The devices' setters (roadmap step S5.20): every writer of a device, a container or a chain of a live song works its next
// value out and sets it through these, the only functions (with the keeper and the versions' pass) that set the devices'
// area (SongValues.h: setDevice(), eraseDevice(), setDeviceListOrder(), setAllDevices()), in the mode of the moment: in the
// step in progress, or outside the history while the song is written outside it (Song::OutsideHistory). Each computes the
// area's next value (immer: a path copied, the rest shared) and sets it once; the narrow ones change one entry of one map. A
// value equal to what the song holds is no change, and makes no action. None begins a step. Each finds what it writes by a
// look in the area, never a walk of the song. Each returns whether it changed the song.
//   - setDevice(): a device, a container or a chain made this value: one the song hasn't added at its place, at the index
//     given in its list (-1: after the others there), as setNewDevices() adds one; one it has keeps its place in its list,
//     but where the value's place is another list, to which it moves at the index given, with all it holds (a container's
//     chains and their devices; what the store holds of each and its window's place, by its ID, stay its own). One held as
//     another form of a device (one of Phazite's own for a plug-in, or the other way) is made anew at the index given; one
//     whose ID the song holds as another kind of item (a device, a container, a chain: each against the other two) is
//     refused, the song left as it was, as it can't hold two items of one ID; and one whose place names an owner the song
//     hasn't (a channel, a container, a chain) too.
//   - setNewDevices(): what a maker made (MadeDevices), new: the first at its place, at the index given in its list, the
//     rest in it (a container's chains, their devices; a chain's devices), each in its list in their order, with the store's
//     entries of each (a plug-in's state and program, a chain's mute and solo), in one set of the area.
//   - eraseDevice(): one taken out, with what it holds; by its ID, or at its place (findDevicePlace()), where it must be.
//   - setDeviceListOrder(): an owner's list (a channel's devices, a container's chains, a chain's devices) given an order of
//     the same IDs (else nothing).
//   - moveInDeviceList(): one moved within its list to the place of the one there (after it moving later, before it moving
//     earlier); inChain, only a device in a chain, never before its instrument nor an instrument itself.
//   - moveDevices(): several moved from one owner's list into another's at an index (-1: after those there), in their order,
//     each with what it holds, each with its fields edited too (a sidechain turned off: setDeviceFields()).
//   - setDeviceFields(), setChainFields(): a device's or a container's (a chain's) own fields edited (its values and sounds
//     kept, whatever the edit does to them): its switch, a mix, a sidechain, a name, a split, a chain's fader and ranges.
//   - setDeviceValue(): one of Phazite's own device's values set by hand, by its index among its type's parameters, held as
//     the song holds it (SavedValues.h: keepHeldValue(): within its range, on its steps, none at its default), one entry of
//     its values' map; and setValueSetByHand() (Parameters.h) passes the value so held on to a curve's lone point it moves
//     (model/automation/internal/Automation.h: moveOnlyPoint()) and to what is told of it.
//   - setDeviceSound() (below): one of its sounds, empty for none, one entry of its sounds' map.
struct MadeDevices;

bool setDevice (Song&, DeviceValue, int index = -1);
bool setNewDevices (Song&, const MadeDevices&, int index = -1);
bool eraseDevice (Song&, DeviceID);
bool eraseDevice (Song&, DeviceID, const DevicePlace& at);
bool setDeviceListOrder (Song&, const DevicePlace& owner, immer::flex_vector<DeviceID> order);

enum class ListMove
{
    anywhere, // a device or a container on its channel, or a device in its chain
    inChain,  // a device in its chain, never its instrument nor before it
    chains    // a chain among its container's
};

bool moveInDeviceList (Song&, DeviceID, int newIndex, ListMove = ListMove::anywhere);
bool moveDevices (Song&, const std::vector<DeviceID>&, const DevicePlace& from, const DevicePlace& to, int index = -1,
                  const std::function<void (DeviceValue&)>& edit = {});
bool setDeviceFields (Song&, DeviceID, const std::function<void (DeviceValue&)>& edit);
bool setChainFields (Song&, DeviceID chain, const std::function<void (DeviceValue&)>& edit);
bool setDeviceValue (Song&, DeviceID, int parameterIndex, float value);

// Where a device or a container is, and a chain: a look in the area; none for one the song hasn't. A channel's devices,
// containers, chains and the devices in them, in the outline's order (getDeviceOutline()): for a copy of them
// (copyDevices()). And a device's name as getDeviceName() gives one, of its value.
std::optional<DevicePlace> findDevicePlace (const Song&, DeviceID);
std::optional<DevicePlace> findChainPlace (const Song&, DeviceID chain);
std::vector<DeviceValue> getChannelDeviceValues (const Song&, TrackID channel);
juce::String getDeviceName (const DeviceValue&);

// The place of a channel's devices' list, by its track's ID, the master's by its ID or none.
DevicePlace getDeviceChannelPlace (const Song&, TrackID channel);

//==============================================================================
// What a maker makes, on no channel yet, with IDs of the song's: the devices (the first is what is placed; the rest are in it,
// each placed in its own container and chain: a container's chains, each followed by its devices), the store's entries of
// any of them (a preset's plug-in's state and program, a chain's mute and solo), and a container's eight macros as
// modulation's values (ModulationValue.h), which its inserter sets through modulation's setter in the same step, right after
// the container is in the song. Moved into an inserter, which places it: read nothing of it after.
struct MadeDevices
{
    std::vector<DeviceValue> devices {};
    DeviceStore store {};
    std::vector<ModulatorValue> macros {};
};

// One of Phazite's own devices (by its type ID), with values set by hand by its parameters' IDs, each held as the song holds
// it (SavedValues.h: keepHeldValue()); or a device of a type the browser and the menus offer (DeviceType.h): one of
// Phazite's own, or a plug-in by its description (Schema.h: pluginFormat to pluginInstrument). And a plug-in's identity of a
// description.
MadeDevices makeOwnDevice (Song&, std::string_view typeID, const std::vector<std::pair<juce::String, float>>& values = {});
MadeDevices makeDevice (Song&, const DeviceType&);
PluginIdentity fromDescription (const juce::PluginDescription&);

// Puts what a maker made on a channel, at a place among its devices (-1: after them), an instrument first; a plain folder
// becomes a bus first; a container's macros set right after it. What plays it is made from it. Joins the caller's step.
// Returns its ID, or none for a channel that can't hold it.
DeviceID insertDevice (Song&, TrackID channel, MadeDevices, int index = -1);

// Puts an instrument on a track, in place of the one it had, in the caller's step. Returns it, or none if the track can't
// hold one (a folder, a return or the master).
DeviceID setInstrument (Song&, TrackID, MadeDevices instrument);

// Each copied track (by the copied IDs: a duplicate's, model/arrangement/internal/Tracks.h, duplicateTracks()) given its
// original's devices, containers and chains and what they hold, in their order, each by the ID the copied IDs give it (a new
// one of the song's where they give none, added to them), a sidechain that listens to a track copied too listening to the
// copy, what the store holds of each and its window's place copied (DeviceStore.h); before what names them is copied by the
// copied IDs (the sends, modulation with a container's macros, the curves). Nothing for a copy that holds devices. In the
// caller's undo step.
void copyDevices (Song&, CopiedIDs& copied);

// What a track's removal takes of the devices (model/arrangement/internal/Tracks.h: eraseTrackHoldings()): every device,
// container and chain on these channels (tracks by their IDs, the master by its), in one set of the devices' area, each
// channel's list looked in alone, nothing walked of the rest; what the store holds of each stays (DeviceStoreArea.h). Whether
// it changed the song's devices. And the devices, containers and chains a channel's element holds (its DEVICE and CONTAINER
// children, each container's CHAINs and their DEVICEs, not those of the tracks in it) given to the song's channel of its ID
// (a merge's choice of a whole track or the master: Tracks.h, chooseTrackClash()), as the keeper takes in those of a channel
// that comes into the song with them (model/base/internal/DevicesArea.h): each read as the song holds one (DevicesValue.h:
// readHeldDevice()), in their order, after the channel's others, one of an ID the song holds left out with what it holds, in
// one set of the area.
bool eraseChannelDevices (Song&, const std::vector<TrackID>& channels);
void takeDevicesOf (Song&, const juce::ValueTree& channel);

// Removes a device, as one undo step named after it: a container with what it holds (Containers.h), and a device in a
// container from its row, what it holds of its own stored first (Song::storeDeviceStates()), so that an undo brings it
// back as it was.
// A bus left without devices becomes a plain folder again, unless it was made a bus on purpose. What named the device goes
// with it (forgetWhatNamesTheGone()).
void removeDevice (Song&, DeviceID);

// The IDs of the items the song holds that something can name (a curve, a modulation, a MIDI link, a send): its tracks,
// their faders and VCAs and the master's, its devices, containers and chains, its sends,
// modulators and macros; from its tree (Schema.h: holdsID(), its clips and its automation aside, which hold none that these
// name) and its values (IDsHeld.h: forEachNameableIDHeld()), a song without values from its tree alone. One walk of the
// song, gathered once for a pass that asks of many: the one answer to "does the song hold what this names", which
// forgetWhatNamesTheGone() asks at every removal and automation asks of a curve's owner (Automation.cpp:
// isCurveOwnerThere(), the loader's too, makeSong()).
std::set<std::uint64_t> getItemIDs (const Song&);

// Takes what names an item the song no longer holds out of it, in the caller's undo step: the modulations, the MIDI links
// and the automation curves of a device's, a chain's, a send's, a fader's, a modulator's or a macro's parameters
// (Automation.h: forgetCurvesOfTheGone()); the sends to a return; and the tracks of a drum sampler's pads, which go with
// their sampler as a Drum Rack's chains go with it in Ableton Live (one holding clips stays, a plain track, so that no clip
// is lost). Their lanes, view state, stay, ignored while what they name is gone, and go as the project opens (Schema.h:
// LANE); the sidechains that listened to a track are forgotten as it goes (Sidechain.h). Every change
// that takes a device, a chain, a send, a fader or a track out of the song calls it, so that the song stays as
// makeConsistent() (model/project/internal/Project.h) would leave it. A pad's track that stays loses its pad fields through
// the tracks' setters (model/arrangement/internal/Tracks.h: setTrackFields()), in the mode of the moment: outside the history
// while the song is written outside it (the code before the tracks' writers always wrote them through the undo manager).
void forgetWhatNamesTheGone (Song&);

// Moves a device to another place among its channel's devices, as one undo step.
void moveDevice (Song&, DeviceID, int newIndex);

// Turns a device on or off, as one undo step; and whether it is on.
void setDeviceOn (Song&, DeviceID, bool on);
bool isDeviceOn (const Song&, DeviceID);

// One of a device's files or tables (Schema.h: SOUNDS; empty for none), in the caller's undo step.
void setDeviceSound (Song&, DeviceID, const juce::Identifier& property, const juce::String& value);

// (Where a device's own window was last, view state the project saves so that the window opens there again (docs/design.md,
// section 9), is in the store's block: model/plugins/internal/DeviceStore.h, getWindowPlace() and setWindowPlace().)

// An effect's mix: how much of what it puts out is its effect, from 0 (only the sound that comes into it, "dry") to 1
// (only the effect, "wet"), as FL Studio's mixer slots and REAPER's effect chains have for every effect: one of Phazite's
// own effects' mix parameter, or a plug-in's or a container of effects' mix (Schema.h: deviceMix). An instrument (an
// instrument container too) has none. Set in the caller's step.
bool hasMix (const Song&, DeviceID);
float getMix (const Song&, DeviceID);        // 1 for a device without a mix
float getDefaultMix (const Song&, DeviceID); // its mix parameter's default (a reverb's 0.25), else 1
void setMix (Song&, DeviceID, float mix);

// The description of the plug-in a copy's element names, as the element has it (read through the conversion: DevicesValue.h,
// readPluginIdentity(); a preset's), and of a plug-in's identity as the devices' values hold it (fromDescription() above the
// other way; the plug-ins' instances, PluginInstances.h).
juce::PluginDescription getPluginIdentity (const juce::ValueTree& device);
juce::PluginDescription toDescription (const PluginIdentity&);

//==============================================================================
// Adds a device dropped from the browser or chosen in the plug-in picker, as one named undo step. An instrument becomes a
// track's instrument, in place of the one it had; a track that can't hold one (a folder or a return), or none, gets a
// new track after it, named and colored after the instrument, and so does a track with audio clips, which an
// instrument would silence. An effect goes after the track's effects, or with no track on the master. Returns the
// device, or none.
DeviceID addDeviceTo (Song&, TrackID, const DeviceType&, Selection*);

// The same for a device made already (from a preset), joining the caller's step.
DeviceID placeDevice (Song&, TrackID, MadeDevices device, Selection*);

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
// pan", or a VCA's "vca"). And the channel whose fader or VCA a device is (a track by its ID, the master by its flag:
// ChannelRef, model/base/IDs.h), none for another device: a walk of the tracks and the master, one field of each read.
DeviceID getFader (const Song&, TrackID channel);
juce::String getVolumeParameterID (const Song&, TrackID channel);
juce::String getPanParameterID (const Song&, TrackID channel); // none for a plain folder's VCA
ChannelRef findChannelOfFader (const Song&, DeviceID fader);

// The same among tracks given, in the order given, then their master: what a loader that read a song without values once
// asks (model/devices/internal/Parameters.h: locateParameter() with the song's devices and tracks given).
ChannelRef findChannelOfFaderIn (const SongTracks&, const std::vector<TrackID>& order, DeviceID fader);

// A channel's volume, in decibels, and its pan, from -1 to 1 (Schema.h: channelVolume, channelPan): the song's, which its
// fader plays (a plain folder's VCA has no pan); the master's are the master's, whose volume is -3 dB until it is set, as a
// new song's. Set in the caller's undo step.
inline constexpr float defaultMasterVolume = -3.0f;

// The range of a fader's volume, and of a send's level: silence (-100 dB, and nothing below) to the top of its travel (+6
// dB: engine/nodes/Channel.h). What a file or a preset says is kept within it as it comes in, whoever wrote it; no number
// at all is 0 dB.
inline constexpr float lowestVolume = -100.0f, highestVolume = 6.0f;
float keepVolumeInRange (float decibels);

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
TrackID addReturnWithEffect (Song&, TrackID sender, MadeDevices effect);

// A track's sends (Schema.h: SEND; the song's sends' area, SongValues.h), each to a return by the return's track, in their
// order; and adding one from a track to a return: after the fader, or before it with preFader, its level starting at -6 dB.
// Returns none if the track can't send there or the return isn't one. Joins the caller's step.
std::vector<DeviceID> getSends (const Song&, TrackID);
DeviceID addSend (Song&, TrackID from, TrackID returnTrack, bool preFader);

// The sends as values (model/base/internal/SendsValue.h), which every reader of a send reads them as: a send by its ID (none
// for one the song hasn't), the track it is on (none for none), a track's sends in their order (none for the master, or a
// track the song hasn't or that sends nowhere), the return a send goes to, and a track's (first) send to a return, none for
// none. Each a look in the sends' area of the song's values (SongValues.h: sends), by the send's ID or the track's; for a song
// that holds no values (the loader's song, a check's copy), a walk of its tree. A control's parameter finds its send by its
// ID, at each tick (model/devices/internal/Parameters.h: FoundParameter), and a strip's knob its track's send to a return
// (findSendFromTo(): model/mixer/MixerModel.h, getSendLevelTo()), nothing allocated.
std::optional<SendValue> getSendValue (const Song&, DeviceID send);
TrackID getSendTrack (const Song&, DeviceID send);
std::vector<SendValue> getSendValues (const Song&, TrackID);
DeviceID findSendFromTo (const Song&, TrackID from, TrackID returnTrack);
TrackID getSendTarget (const Song&, DeviceID send);
bool isPreFader (const Song&, DeviceID send);
void setPreFader (Song&, DeviceID send, bool); // as one undo step, "Change Send"

// A send's level in decibels, the song's, set in the caller's step within a fader's range, to a ten-thousandth.
void setSendLevel (Song&, DeviceID send, float decibels);
float getSendLevel (const Song&, DeviceID send);

// The send menus' choices, each one undo step named "Add Send": a send after the fader to a return, or to a new, empty
// return. Each returns the send, or none. And removing one, "Remove Send".
DeviceID addSendTo (Song&, TrackID from, TrackID returnTrack);
DeviceID addSendToNewReturn (Song&, TrackID from);
void removeSend (Song&, DeviceID send);

// What a track's removal takes of the sends (model/arrangement/internal/Tracks.h: eraseTrackHoldings()): every send on these
// tracks, in one set of the sends' area (SongValues.h: eraseSends()), each track's list looked in alone, nothing walked of
// the rest; whether it changed the song's sends. Not the sends to a return that goes: forgetWhatNamesTheGone()'s.
bool eraseTrackSends (Song&, const std::vector<TrackID>&);

// The sends a track's element holds (its SEND children, not those of the tracks in it) given to the song's track of its ID (a
// merge's choice of a whole track: Tracks.h, chooseTrackClash()), as the keeper takes in those of a track that comes into the
// song with them (model/base/internal/SendsArea.h): each read as the song holds one (SendsValue.h: readHeldSend()), in their
// order, after the track's others, one of an ID the song holds left out.
void takeSendsOf (Song&, const juce::ValueTree& track);

// Each copied track (by the copied IDs: a duplicate's, model/arrangement/internal/Tracks.h, duplicateTracks()) given its
// original's sends, in their order, each with a new ID of the song's, which joins the copied IDs before what names a send's
// level is copied by them (a modulation: Modulation.h, copyModulation(); a curve: Automation.h, copyCurves()); a send to a
// return copied too goes to the copy, as a copy names what it holds. Nothing for a copy that holds sends already. In the
// caller's undo step, the sends set once.
void copySends (Song&, CopiedIDs& copied);

// The mixer's commands: add a return track.
void addMixerCommands (CommandRegistry&, Selection&);

// A device as a slot of the open project's shows it (MixerModel.h): with how late its sound comes out, whether its plug-in
// crashed, whether it is frozen with its track (FreezeModel.h), and whether its window shows (the project context's
// DeviceWindowHost).
DeviceSlot makeDeviceSlot (const ProjectContext&, DeviceID);

//==============================================================================
// The devices that can be added (DeviceType.h): Phazite's own and the plug-ins found by scanning, which the plug-in host
// keeps (model/plugins/internal/PluginHost.h). The plug-in scanning found that a description names: by its identifier and
// name, else by its name, maker and format, as a preset finds it.
std::vector<DeviceType> getDeviceTypes (const PluginHost&);
std::optional<juce::PluginDescription> findKnownPlugin (const PluginHost&, const juce::PluginDescription&);

// A plug-in's key parameters, by its own IDs (MixerModel.h): its first parameters that can be automated, as its instance
// tells them, up to so many. None for a plug-in that doesn't play (with no instance), for Phazite's own devices, whose
// editors show all of theirs, or for a container, which shows its macros.
juce::StringArray getKeyControls (const Song&, DeviceID, int maximum);
} // namespace phazite
