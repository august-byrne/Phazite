// SPDX-License-Identifier: AGPL-3.0-or-later
// The drum sampler's pads (devices/drums/DrumSampler.h): what each plays, for its editor, the piano roll's drum rows and
// sounds dropped from the browser onto them, kept in its device's SOUNDS in the song (Schema.h): one of the factory sounds by
// its name (schema::getPadSound()), and a sound file that plays instead while it is there (schema::getPadSoundFile(), a path
// within the project's folder).
#pragma once

#include "devices/drums/DrumSampler.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"

namespace phazite
{
// Whether a device is Phazite's drum sampler, by its element in the song or its ID.
bool isDrumSampler (const juce::ValueTree& deviceState);
bool isDrumSampler (const Song&, DeviceID);

// What a pad plays: its sound file's name, or its factory sound's ("808 Kick", or the default kit's "Kick"); and whether
// it plays a file.
juce::String getPadName (const Song&, const juce::ValueTree& deviceState, int pad);
juce::String getPadName (const Song&, DeviceID, int pad);
bool hasPadSound (const Song&, DeviceID, int pad);

// Puts a sound file on a pad, as one undo step named after it: the project plays its own copy, in its Audio folder
// (model/arrangement/internal/Import.h). Returns false, changing nothing, for a file that isn't a sound Phazite can read.
bool setPadSound (Song&, DeviceID, int pad, const juce::File&);

// Takes a pad's sound file away, as one undo step: it plays its kit's sound again, the factory sound a preset put there or
// the default kit's.
void clearPadSound (Song&, DeviceID, int pad);

// Loads what a pad plays into a drum sampler, as its device's SOUNDS in the song name it: its file, if it can be read, or
// else the factory sound it names, or its own in the default kit.
void loadPadSound (devices::DrumSampler&, const Song&, const juce::ValueTree& sounds, int pad);
} // namespace phazite
