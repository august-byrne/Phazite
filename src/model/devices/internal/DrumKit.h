// SPDX-License-Identifier: AGPL-3.0-or-later
// The drum sampler's pads (devices/drums/DrumSampler.h): what each plays, for its editor, the piano roll's drum rows and
// sounds dropped from the browser onto them.
#pragma once

#include "model/base/internal/Tracktion.h"

namespace phazite
{
// Whether a device is Phazite's drum sampler.
bool isDrumSampler (const te::Plugin&);

// What a pad plays: its sound file's name, or the default kit sound's ("Kick"); and whether it plays a file.
juce::String getPadName (te::Plugin&, int pad);
bool hasPadSound (te::Plugin&, int pad);

// Puts a sound file on a pad, as one undo step named after it: the project plays its own copy, in its Audio folder
// (model/arrangement/internal/Import.h). Returns false, changing nothing, for a file that isn't a sound Phazite can read.
bool setPadSound (te::Plugin&, int pad, const juce::File&);

// Puts the default kit's sound back on a pad, as one undo step.
void clearPadSound (te::Plugin&, int pad);

// Drum samplers' states as a preset keeps them (model/presets/internal/Presets.h): their pads' sounds by their full
// paths, so that another project can copy them in. And the other way, drum samplers' states from a preset made a
// project's: their sounds copied into its Audio folder, and those that aren't there any more left to the kit's. Each
// goes through a state and everything in it (a container's, a track's), and isn't an undo step: the state isn't in a
// project.
void writePadSoundPaths (const te::Edit& from, juce::ValueTree& state);
void copyPadSoundsInto (te::Edit&, juce::ValueTree& state);
} // namespace phazite
