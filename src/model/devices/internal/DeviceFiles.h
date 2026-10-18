// SPDX-License-Identifier: AGPL-3.0-or-later
// The sound files Phazite's own devices play, which their states keep by path: a drum sampler's pads (DrumKit.h) and a
// wavetable synth's oscillators (Wavetables.h). A project keeps its own copy of each, in its Audio folder
// (model/arrangement/internal/Import.h), named by its path within the project's folder.
#pragma once

#include "model/base/internal/Tracktion.h"

namespace phazite
{
// The path a device keeps for a file: within the project's folder, written the same on every system, or the file's full
// path for one outside it (or while the project has no file). And the file a kept path names, or none.
juce::String getProjectPath (const te::Edit&, const juce::File&);
juce::File findProjectFile (const te::Edit&, const juce::String& path);

// Devices' states as a preset keeps them (model/presets/internal/Presets.h): their files by their full paths, so that
// another project can copy them in. And the other way, devices' states from a preset made a project's: their files copied
// into its Audio folder, and those that aren't there any more left out, so that the device plays its own sound (a kit's,
// or a factory table). Each goes through a state and everything in it (a container's, a track's), and isn't an undo step:
// the state isn't in a project.
void writeDeviceFilePaths (const te::Edit& from, juce::ValueTree& state);
void copyDeviceFilesInto (te::Edit&, juce::ValueTree& state);
} // namespace phazite
