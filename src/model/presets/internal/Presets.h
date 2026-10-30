// SPDX-License-Identifier: AGPL-3.0-or-later
// Presets' Tracktion side (model/presets/PresetModel.h), for the model, the app and tests; not for the UI: a preset as its
// file holds it, in Phazite's own names (Schema.h: schema::preset), made from a device or a track, read and written, and
// what it holds made into Tracktion's objects in an Edit (roadmap A5, 2026-10-30).
#pragma once

#include "model/base/internal/Tracktion.h"

namespace phazite
{
// A preset of a device (of a container: its devices and macros) or of a track, as PresetModel.h says what each holds; void
// for a device that is neither one of Phazite's own, a plug-in nor a container.
juce::var makeDevicePreset (te::Plugin&);
juce::var makeTrackPreset (te::AudioTrack&);

// A preset file's contents; void for a file that isn't a preset. And writing one, replacing what the file held; true if
// it did.
juce::var readPreset (const juce::File&);
bool writePreset (const juce::var& preset, const juce::File&);

// What a preset holds, made anew in an Edit: a device's or a container's on a channel (a track's, or the master's with
// none) as a dropped device goes (Mixer.h: placeDevice()), and a track's as a new track after another (at the end with
// none). Each returns what it added, or nullptr (a plug-in that isn't installed, say), and joins the caller's undo step.
te::Plugin* addDeviceFromPreset (te::Edit&, te::Track*, const juce::var& preset, te::SelectionManager*);
te::AudioTrack* addTrackFromPreset (te::Edit&, te::Track* after, const juce::var& preset, te::SelectionManager*);
} // namespace phazite
