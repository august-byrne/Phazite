// SPDX-License-Identifier: AGPL-3.0-or-later
// The drum sampler's pads (devices/drums/DrumSampler.h): what each plays, for its editor, the piano roll's drum rows and
// sounds dropped from the browser onto them.
#pragma once

#include "model/base/internal/Tracktion.h"

namespace phazite
{
// Whether a device is Phazite's drum sampler.
bool isDrumSampler (const te::Plugin&);

// What a pad plays: its sound file's name, or its factory sound's ("808 Kick", or the default kit's "Kick"); and whether
// it plays a file.
juce::String getPadName (te::Plugin&, int pad);
bool hasPadSound (te::Plugin&, int pad);

// Puts a sound file on a pad, as one undo step named after it: the project plays its own copy, in its Audio folder
// (model/arrangement/internal/Import.h). Returns false, changing nothing, for a file that isn't a sound Phazite can read.
bool setPadSound (te::Plugin&, int pad, const juce::File&);

// Takes a pad's sound file away, as one undo step: it plays its kit's sound again, the factory sound a preset put there or
// the default kit's.
void clearPadSound (te::Plugin&, int pad);
} // namespace phazite
