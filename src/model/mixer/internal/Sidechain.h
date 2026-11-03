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

// The tracks a device's sidechain can listen to, in the song's order: every track with sound of its own (not a folder)
// but the device's own and those whose sound comes from the device's track (through a bus, a send or another sidechain),
// which would make a loop.
std::vector<TrackID> getSidechainSources (const Song&, DeviceID);

// The track a device's sidechain listens to, or none when it listens to the sound passing through it; and the same, with
// its name and colour, as the Devices view and the mixer show it.
TrackID getSidechainSource (const Song&, DeviceID);
SidechainChoice describeSidechainSource (const Song&, DeviceID);

// Every track with sound of its own but one, in the song's order, with its colour, and why a device on that one couldn't
// listen to it: its sound comes from that track, which would make a loop (getSidechainSources()). With none, every
// track, as a device on the master hears them. describeSidechainChoices() gives them for a device's sidechain, if it has
// one.
std::vector<SidechainChoice> describeTracksToListenTo (const Song&, TrackID listener);
std::vector<SidechainChoice> describeSidechainChoices (const Song&, DeviceID);

// Makes a device's sidechain listen to a track, one of getSidechainSources(), or with none to the sound passing through
// it, as one undo step. Phazite's compressor, still as it was made, starts ducking then, in the same step: quick to clamp
// down (1 ms), slow to let go (150 ms), deep (8:1 from -30 dB); and hearing only below 150 Hz when the source plays a drum
// sampler, so that a kick ducks it and the snare and hats don't.
void setSidechainSource (Song&, DeviceID, TrackID source);

// Puts Phazite's compressor after a track's effects, ducking under another track (setSidechainSource()), as one undo step
// ("Duck Bass Under Kick"). Returns the compressor, or none when the key would make a loop.
DeviceID duckUnder (Song&, TrackID ducked, TrackID key);

// Points the sidechains that listen to these tracks, or to tracks inside them, back at the sound passing through each
// device: for tracks about to be deleted. Joins the caller's undo step.
void forgetSidechainSources (Song&, const std::vector<TrackID>& tracks);
} // namespace phazite
