// SPDX-License-Identifier: AGPL-3.0-or-later
// The sampler's sound (devices/sampler/Sampler.h; docs/design.md, section 9): what it plays, for its editor, and choosing
// one of its factory sounds or a sound file for it, each as an undo step that also sets what belongs to the sound: its root
// key (to the sound's pitch), its markers (to the whole sound) and its loop (to the file's own, where it has one); where it
// is sliced, and the slices moved, added and removed by hand; and its slices written as notes. Kept in its device's SOUNDS
// in the song (Schema.h: samplerSample, samplerSampleFile, samplerSlices). Message thread only.
#pragma once

#include "devices/sampler/Sampler.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"

#include <memory>
#include <optional>
#include <vector>

namespace phazite
{
class Selection;

// Whether a device is Phazite's sampler, by its element in the song or its ID.
bool isSampler (const juce::ValueTree& deviceState);
bool isSampler (const Song&, DeviceID);

// What it plays: its sound file's name, or its factory sound's ("808 Bass"); whether it plays a file; and which factory
// sound it plays (devices/sampler/FactorySamples.h), or -1 while it plays a file.
juce::String getSampleName (const Song&, DeviceID);
bool hasSampleFile (const Song&, DeviceID);
int getFactorySampleIndex (const Song&, DeviceID);

// What a sound file says of how it plays (for setSampleFile(), and tests): the note that plays it at its own pitch, from
// the note its own data name (a WAV file's smpl chunk), else a note in its name, in the octave its pitch is in (or from
// middle C), else its pitch (devices/sampler/SampleAnalysis.h), else middle C; the cents its pitch is off that note, which
// Fine takes back (a sound tuned between notes is played in tune); and the loop its own data hold, as parts of its length.
struct SampleSettings
{
    enum class RootFrom
    {
        file,
        name,
        pitch,
        none
    };

    int root = 60;
    float fine = 0.0f; // cents, within half a semitone
    RootFrom rootFrom = RootFrom::none;
    std::optional<std::pair<float, float>> loop;
};

std::optional<SampleSettings> readSampleSettings (const juce::File&);

// Puts a sound file on the sampler, as one undo step named after it ("Put Vox Chop on the Sampler"): the project plays its
// own copy, in its Audio folder (model/arrangement/internal/Import.h), at the root key and fine readSampleSettings() finds,
// its markers on the whole sound, its loop the file's own (on) or off where it starts, and no slices changed by hand; the
// mode and everything else as they were. Returns false, changing nothing, for a file that isn't a sound Phazite can read.
bool setSampleFile (Song&, DeviceID, const juce::File&);

// One of its factory sounds, as one undo step named after it, at the sound's root key, its markers on the whole sound, its
// loop off and no slices changed by hand; nothing if it plays that one already.
void setFactorySample (Song&, DeviceID, int index);

// Sets its root key and fine from the sound it plays, as readSampleSettings() finds them, as one undo step ("Find Root
// Key"); nothing for a sound with no note of its own, which keeps the root key it has.
void findRootKey (Song&, DeviceID);

// A sound file as the sampler plays it (its first longestSampleSeconds), with where its hits begin and, for a loop that
// says, how many beats it lasts; kept while anything holds it, so that a file is read once, however many samplers play it.
// nullptr for one that isn't a sound Phazite can read.
inline constexpr double longestSampleSeconds = 120.0;
std::shared_ptr<const devices::Sampler::Sound> readSampleSound (const juce::File&);

// The sound it plays, for drawing it (nullptr for a device that isn't a sampler), and its slices changed by hand.
std::shared_ptr<const devices::Sampler::Sound> getSampleSound (const Song&, DeviceID);
devices::Sampler::ManualSlices getManualSlices (const Song&, DeviceID);

// Where it is sliced now, as its editor shows it: the slices' starts after the first, as parts of the sound's length.
std::vector<float> getSlicePositions (const Song&, DeviceID);

// A slice moved, added or taken away by hand, each one undo step ("Move Slice", "Add Slice", "Remove Slice"). A slice put
// in by hand stays whatever slices the sound (its transients at any sensitivity, beats or equal parts), and one it found
// that is moved or taken away stays away, as Simpler's, Logic's and Bitwig's do (docs/design.md, section 9). A slice is
// counted from 1: the first starts at the start marker, which slices can't move.
void moveSlice (Song&, DeviceID, int slice, float position);
void addSlice (Song&, DeviceID, float position);
void removeSlice (Song&, DeviceID, int slice);

// Its slices as notes, each on its slice's note as long as its slice, in a new MIDI clip on its track at the play start
// (or after the track's last clip, where one is in the way), so that the clip plays the sound as it was, in time with the
// song: one undo step, "Slices to Clip". Returns the clip, selected if a selection is given; none for a device that isn't
// a sampler on a track.
ClipID writeSliceNotes (Song&, DeviceID, Selection*);

// Loads what its device's SOUNDS in the song name into a sampler: its file, if it can be read, or else the factory sound it
// names, or the first; and its slices changed by hand.
void loadSamplerSound (devices::Sampler&, const Song&, const juce::ValueTree& sounds);
void loadSamplerSlices (devices::Sampler&, const juce::ValueTree& sounds);
} // namespace phazite
