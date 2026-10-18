// SPDX-License-Identifier: AGPL-3.0-or-later
// Presets (docs/design.md, section 9; roadmap step 2.4), by IDs and plain values: a device's settings, a container's
// devices and macros, or a track's devices, modulators and mix, in a file of their own (".phazitepreset") that any
// project can take in. Each function takes the open project's context, and gives nothing, or does nothing, for an ID of
// something that is gone. The Tracktion side is internal/Presets.h.
#pragma once

#include "model/base/IDs.h"

#include <juce_core/juce_core.h>

#include <optional>

namespace phazite
{
class ProjectContext;

enum class PresetKind
{
    device,    // an instrument or an effect
    container, // a container of effects (model/mixer/ContainerModel.h)
    track      // a track's devices, modulators and mix
};

// What a preset holds, from its file.
struct PresetInfo
{
    PresetKind kind = PresetKind::device;
    juce::String device {};    // a device preset's device: "EQ", or a plug-in's name
    bool isInstrument = false; // a device preset's device is an instrument
};

inline constexpr const char* presetExtension = ".phazitepreset";
bool isPresetFile (const juce::File&); // by its extension

// What a preset file holds, from its first element; nothing for a file that isn't a preset.
std::optional<PresetInfo> readPresetInfo (const juce::File&);

// The folder a device's presets go in, among the user's presets: the device's name ("EQ"), or "Containers"; and the one
// tracks' go in.
juce::String getPresetFolderName (const ProjectContext&, DeviceID);
inline constexpr const char* trackPresetsFolder = "Tracks";

// Writes a preset of a device (a container is one) or of a track into a file, replacing what it held, and returns
// whether it did. A device's holds its settings and its mix: not its automation, what modulates it from outside, its
// sidechain's source or whether it is on. A container's holds its devices and its macros with what they modulate in it.
// A track's holds its name, color, volume and pan, its devices and containers, and its modulators and macros with what
// they modulate on it: not its clips, automation, sends or what goes into it. The sound files a device plays (a drum
// sampler's pads', a wavetable synth's tables) go by their paths, for another project to copy in.
bool saveDevicePreset (const ProjectContext&, DeviceID, const juce::File&);
bool saveTrackPreset (const ProjectContext&, TrackID, const juce::File&);

// Puts what a preset holds into the song, as one undo step named after the preset ("Add Vocal Chain"), for a track (or
// with an empty ID, below the tracks): a device's or a container's goes where a device dropped there would
// (MixerModel.h: addDeviceTo()), an effect after the track's effects or on the master, an instrument in place of the
// track's or on a new track; a container is named after the preset. A track's goes after the track, as a new one, or at
// the end. Returns whether anything went in.
bool loadPreset (ProjectContext&, const juce::File&, TrackID);

// Puts a device preset's settings and mix into a device with a window of its own (a plug-in, or one of Tracktion's
// built-in devices: MixerModel.h's DeviceSlot::opensWindow) of the preset's kind (the same plug-in, or the same device),
// in place of its own, as one undo step named after the preset ("Load Preset Warm Pad"): a plug-in window's Load Preset
// (step 2.12). Returns whether it did: not for a preset of another device, nor of a container or a track.
bool loadPresetInto (ProjectContext&, DeviceID, const juce::File&);
} // namespace phazite
