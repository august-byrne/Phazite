// SPDX-License-Identifier: AGPL-3.0-or-later
// Sidechains (docs/design.md, section 9): a device that listens to another track's sound rather than to the sound
// passing through it, such as a compressor that turns a bass down while the kick plays ("ducking"). Phazite's devices
// that have one say so (devices/core/DeviceInfo.h); a plug-in has one if it has inputs for one. The source keeps
// feeding the sidechain while muted, so a muted "ghost" kick can still duck the bass.
#pragma once

#include "model/base/Tracktion.h"

namespace phazite
{
// Whether a device has a sidechain to route a track into.
bool hasSidechain (te::Plugin&);

// The tracks a device's sidechain can listen to, in the song's order: every audio track but the device's own and those
// whose sound comes from the device's track (through a bus, a send or another sidechain), which would make a loop.
juce::Array<te::AudioTrack*> getSidechainSources (te::Plugin&);

// The track a device's sidechain listens to, or nullptr when it listens to the sound passing through it.
te::AudioTrack* getSidechainSource (te::Plugin&);

// Makes a device's sidechain listen to a track, one of getSidechainSources(), or with nullptr to the sound passing
// through it, as one undo step.
void setSidechainSource (te::Plugin&, te::AudioTrack* source);

// Points the sidechains that listen to these tracks, or to tracks inside them, back at the sound passing through each
// device: for tracks about to be deleted. Joins the caller's undo step.
void forgetSidechainSources (const juce::Array<te::Track*>& tracks);
} // namespace phazite
