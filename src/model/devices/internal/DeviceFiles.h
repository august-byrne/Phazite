// SPDX-License-Identifier: AGPL-3.0-or-later
// The sound files Phazite's own devices play, which their states keep by path: a drum sampler's pads (DrumKit.h) and a
// wavetable synth's oscillators (Wavetables.h). A project keeps its own copy of each, in its Audio folder
// (model/arrangement/internal/Import.h), named by its path within the project's folder.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "model/base/Song.h"
#include "model/base/internal/Tracktion.h"

#include <vector>

namespace phazite
{
// What of one of Phazite's own devices' states names a sound file, a factory sound or a table, besides its parameters: a
// drum sampler's pads', a wavetable synth's oscillators' (Schema.h). None for other devices.
std::vector<juce::Identifier> getSoundProperties (const devices::DeviceInfo&);

// The path a device keeps for a file: within the project's folder, written the same on every system, or the file's full
// path for one outside it (or while the project has no file). And the file a kept path names, or none.
juce::String getProjectPath (const Song&, const juce::File&);
juce::File findProjectFile (const Song&, const juce::String& path);

// Devices' states as a preset keeps them (model/presets/internal/Presets.h): their files by their full paths, so that
// another project can copy them in. And the other way, devices' states from a preset made a project's: their files copied
// into its Audio folder, and those that aren't there any more left out, so that the device plays its own sound (a factory
// sound, or a factory table). Each goes through a state and everything in it (a container's, a track's), and isn't an
// undo step: the state isn't in a project.
void writeDeviceFilePaths (const Song& from, juce::ValueTree& state);
void copyDeviceFilesInto (Song&, juce::ValueTree& state);
} // namespace phazite
