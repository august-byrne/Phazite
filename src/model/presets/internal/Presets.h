// SPDX-License-Identifier: AGPL-3.0-or-later
// Presets (model/presets/PresetModel.h), for the model, the app and tests; not for the UI: a preset as its file holds it,
// in Phazite's own names (Schema.h: schema::preset), made from a device or a track of the song, read and written, and
// what it holds made into the song's elements, with new IDs of the song's (Schema.h: DEVICE, CONTAINER, MODULATOR, MACRO).
#pragma once

#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/base/internal/Selection.h"

namespace phazite
{
class PluginHost;

// A preset of a device (of a container: its devices and macros) or of a track, as PresetModel.h says what each holds, from
// the song, what its plug-ins keep of their own stored into it first (Song::storeDeviceStates()); void for a device that is
// neither one of Phazite's own, a plug-in nor a container, or for one the song hasn't.
juce::var makeDevicePreset (Song&, DeviceID);
juce::var makeTrackPreset (Song&, TrackID);

// A preset file's contents; void for a file that isn't a preset. And writing one, replacing what the file held; true if
// it did.
juce::var readPreset (const juce::File&);
bool writePreset (const juce::var& preset, const juce::File&);

// A device preset's device or a container preset's container as an element of the song's (with its devices, its macros
// and what they modulate), with new IDs, on no channel yet: one of Phazite's own devices with its values and its sounds
// (their files copied into the project), or a plug-in with its own settings and its mix; invalid for a plug-in that the
// plug-in host doesn't know (not installed here), or for a preset of a track.
juce::ValueTree makeDeviceFromPreset (Song&, const PluginHost&, const juce::var& preset);

// What a preset holds, made anew in the song: a device's or a container's on a channel (a track's, or the master's with
// none) as a dropped device goes (Mixer.h: placeDevice()), and a track's as a new track after another (at the end with
// none), with its devices, its modulators and macros and what they modulate on it, its volume and pan. Each returns what it
// added, or none (a plug-in that isn't installed, say), and joins the caller's undo step. What plays the song makes the
// plug-ins' instances.
DeviceID addDeviceFromPreset (Song&, const PluginHost&, TrackID, const juce::var& preset, Selection*);
TrackID addTrackFromPreset (Song&, const PluginHost&, TrackID after, const juce::var& preset, Selection*);
} // namespace phazite
