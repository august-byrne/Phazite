// SPDX-License-Identifier: AGPL-3.0-or-later
// Sidechains (docs/design.md, section 9): a device that listens to another track's sound rather than to the sound
// passing through it, such as a compressor that turns a bass down while the kick plays ("ducking"). Phazite's devices
// that have one say so (devices/core/DeviceInfo.h); a plug-in has one if it has inputs for one. The source keeps
// feeding the sidechain while muted, so a muted "ghost" kick can still duck the bass. The song holds the track each device
// listens to (Schema.h: deviceSidechain).
#pragma once

#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/mixer/SidechainChoice.h"

#include <vector>

namespace phazite
{
// The tracks whose sound comes from a track, the track itself first: those it feeds (the bus it is in, the returns it
// sends to, the tracks whose devices' sidechains listen to it), those they feed, and so on. A route back to any of them
// would make a loop.
std::vector<TrackID> getTracksFedBy (const Song&, TrackID);

// Whether a device has a sidechain to route a track into: not one in a container, which hears what passes through it.
bool hasSidechain (const Song&, DeviceID);

// The tracks a device's sidechain can listen to, in the song's order: every track with sound of its own (a track, a return
// or a bus, not a plain folder, which only scales its tracks' volumes) but the device's own and those whose sound comes from
// the device's track (through a bus, a send or another sidechain), which would make a loop.
std::vector<TrackID> getSidechainSources (const Song&, DeviceID);

// The track a device's sidechain listens to, or none when it listens to the sound passing through it; and the same, with
// its name and colour, as the Devices view and the mixer show it.
TrackID getSidechainSource (const Song&, DeviceID);
SidechainChoice describeSidechainSource (const Song&, DeviceID);

// Whether a device hears the track its sidechain listens to now, as the mixer shows it (a stripe of the track's colour, a
// link to its channel): one that has a source does, but Phazite's compressor only while it ducks (its Duck mode); while it
// compresses, it hears its own sound, its key kept for when it ducks again. And whether a change of a device's value set by
// hand (a property of its VALUES) turns that on or off, as the compressor's mode does.
bool hearsSidechain (const Song&, DeviceID);
bool switchesSidechain (const juce::ValueTree& values, const juce::Identifier& property);

// Every track but one, in the song's order, with its colour, and why a device on that one couldn't listen to it: its sound
// comes from that track, which would make a loop (getSidechainSources()), or it is a plain folder, with no sound of its own.
// With none, every track, as a device on the master hears them. describeSidechainChoices() gives them for a device's
// sidechain, if it has one.
std::vector<SidechainChoice> describeTracksToListenTo (const Song&, TrackID listener);
std::vector<SidechainChoice> describeSidechainChoices (const Song&, DeviceID);

// Makes a device's sidechain listen to a track, one of getSidechainSources(), or with none to the sound passing through
// it, as one undo step. Phazite's compressor ducks under the track chosen, in the same step (its Duck mode), and, still as
// it was made but for its mode, is set up to pump under a kick then: from -14 dB, 14 dB down, in 1 ms, held 120 ms from the
// hit and while the key stays over -14 dB, and back in a straight line over 80 ms; hearing only 60 to 150 Hz when the source
// plays a drum sampler, its kick's body, so that a long kick's boom doesn't hold it down through the beat and the snare,
// clap and hats don't move it. Phazite's multiband, still as it was made, ducks its lows under it. Phazite's vocoder plays
// the track as its carrier (Track).
void setSidechainSource (Song&, DeviceID, TrackID source);

// Puts Phazite's compressor after a track's effects, ducking under another track (setSidechainSource(): its Duck mode), as
// one undo step ("Duck Bass Under Kick"). Returns the compressor, or none when the key would make a loop.
DeviceID duckUnder (Song&, TrackID ducked, TrackID key);

// Points the sidechains that listen to these tracks, or to tracks inside them, back at the sound passing through each
// device: for tracks about to be deleted. Joins the caller's undo step.
void forgetSidechainSources (Song&, const std::vector<TrackID>& tracks);

// The same for the sidechains that listen to a bus itself, as it becomes a plain folder, with no sound of its own
// (Buses.h: makePlainFolder()), or as it goes, its tracks staying (ungroupFolder()). Joins the caller's undo step.
void forgetSidechainsOfBus (Song&, TrackID bus);

// Points the sidechains that a change of where sound goes has made into loops back at the sound passing through each
// device: each that listens to a track whose sound now comes from the device's own, as a track moved into the bus it listens
// to, or a send to the return it listens to, makes it. Phazite's engine can't play a loop, and the device's picker would go on
// naming what it no longer hears (FL Studio refuses such a routing; deleting a track a sidechain listens to turns it off
// alike). Joins the caller's undo step.
void forgetSidechainLoops (Song&);
} // namespace phazite
