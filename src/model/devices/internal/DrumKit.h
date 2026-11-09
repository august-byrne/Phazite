// SPDX-License-Identifier: AGPL-3.0-or-later
// The drum sampler's pads (devices/drums/DrumSampler.h), each by its note, and their layers, from 0: what each plays, for
// its editor, the piano roll's drum rows and sounds dropped from the browser onto them, kept in its device's SOUNDS in the
// song (Schema.h): one of the factory sounds by its name (schema::getPadSound()), and a sound file that plays instead
// while it is there (schema::getPadSoundFile(), a path within the project's folder).
#pragma once

#include "devices/drums/DrumSampler.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"

#include <optional>
#include <utility>

namespace phazite
{
// Whether a device is Phazite's drum sampler, by its element in the song or its ID.
bool isDrumSampler (const juce::ValueTree& deviceState);
bool isDrumSampler (const Song&, DeviceID);

// What a pad's layer plays: its sound file's name, or its factory sound's ("808 Kick", or the default kit's "Kick"), or
// nothing; and whether it plays a file. And whether a pad is empty, none of its layers playing a file or a factory sound
// (as a pad off the default kit's sixteen is until it is given one).
juce::String getPadName (const Song&, const juce::ValueTree& deviceState, int pad, int layer = 0);
juce::String getPadName (const Song&, DeviceID, int pad, int layer = 0);
bool hasPadSound (const Song&, DeviceID, int pad, int layer = 0);
bool isPadEmpty (const Song&, const juce::ValueTree& deviceState, int pad);
bool isPadEmpty (const Song&, DeviceID, int pad);

// Puts a sound file on a pad's layer, as one undo step named after it ("Put Clap on Pad 38's Layer 2"): the project plays
// its own copy, in its Audio folder (model/arrangement/internal/Import.h). Returns false, changing nothing, for a file that
// isn't a sound Phazite can read.
bool setPadSound (Song&, DeviceID, int pad, const juce::File&, int layer = 0);

// Takes a pad's layer's sound away, as one undo step: the first layer's file, after which it plays its kit's sound again,
// the factory sound a preset put there or the default kit's ("Reset Pad 36"); another layer's sound, after which it plays
// nothing ("Clear Pad 36's Layer 2").
void clearPadSound (Song&, DeviceID, int pad, int layer = 0);

// A pad on a mixer channel of its own (roadmap step 4.12), as FL Studio's FPC sends a pad to a mixer track: a track that
// plays it (schema::trackPadDevice and trackPad), with its own effects, sends, fader and the sidechains that listen to it,
// the pad's sound leaving its sampler's track for it. The track that plays a pad, or none; and the sampler and the pad a
// track plays, if it plays one.
TrackID findPadTrack (const Song&, DeviceID, int pad);
std::optional<std::pair<DeviceID, int>> getPlayedPad (const Song&, TrackID);

// Gives a pad a track of its own, after its sampler's track, named after what it plays and in that track's colour, as one
// undo step ("Give Pad 36 Its Own Track"), or the one it has; or brings it back to its sampler's track, deleting its track,
// as one undo step ("Bring Pad 36 Back"). Returns the pad's track, or none.
TrackID givePadItsOwnTrack (Song&, DeviceID, int pad);
void bringPadBack (Song&, DeviceID, int pad);

// Whether a track's drum samplers have pads on tracks of their own, which a freeze or a bounce of it would leave out.
bool hasPadsOnOwnTracks (const Song&, TrackID);

// Loads what a pad's layers play into a drum sampler, as its device's SOUNDS in the song name it: each its file, if it can
// be read, or else the factory sound it names, or the first its own in the default kit, or nothing.
void loadPadSound (devices::DrumSampler&, const Song&, const juce::ValueTree& sounds, int pad);
} // namespace phazite
