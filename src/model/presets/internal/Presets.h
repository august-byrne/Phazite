// SPDX-License-Identifier: AGPL-3.0-or-later
// Presets' Tracktion side (model/presets/PresetModel.h), for the model, the app and tests; not for the UI: a preset as
// its file holds it (Schema.h: PHAZITEPRESET), made of a device or a track, read and written, and what it holds put into
// an Edit with new IDs.
#pragma once

#include "model/base/internal/Tracktion.h"

namespace phazite
{
// A preset of a device (of a container: with its rack type and macros) or of a track (with its containers' rack types),
// as PresetModel.h says what each holds.
juce::ValueTree makeDevicePreset (te::Plugin&);
juce::ValueTree makeTrackPreset (te::Track&);

// A preset file's contents; invalid for a file that isn't a preset. And writing one, replacing what the file held; true
// if it did.
juce::ValueTree readPreset (const juce::File&);
bool writePreset (const juce::ValueTree&, const juce::File&);

// What a preset holds, with new IDs, put into an Edit: a device's or a container's on a channel (a track's, or the
// master's with none) as a dropped device goes (Mixer.h: placeDevice()), and a track's as a new track after another (at
// the end with none). Each returns what it added, or nullptr, and joins the caller's undo step.
te::Plugin* addDeviceFromPreset (te::Edit&, te::Track*, const juce::ValueTree& preset, te::SelectionManager*);
te::AudioTrack* addTrackFromPreset (te::Edit&, te::Track* after, const juce::ValueTree& preset, te::SelectionManager*);
} // namespace phazite
