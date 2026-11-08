// SPDX-License-Identifier: AGPL-3.0-or-later
// What the sampler finds in a sound as it is loaded, off the audio thread (devices/sampler/Sampler.h; docs/design.md,
// section 9): its pitch, so that a note plays it in tune, by YIN (de Cheveigné and Kawahara, 2002) on its steady part past
// its attack; where its hits begin, for slicing it, by the spectral flux of its log magnitudes (Dixon, 2006) over an
// adaptive threshold, each placed where its rise starts; and the zero crossing nearest a place, which its markers snap to,
// so that a sound cut there doesn't click.
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <optional>
#include <vector>

namespace phazite::devices
{
// A hit in a sound: where it begins, in its samples, how strong it is against the strongest, from 0 to 1, and the slicing
// sensitivity from which it slices the sound, from 0 to 1: the stronger it is among the sound's hits, the lower, so that
// turning the sensitivity up adds slices one hit at a time, the strongest first. The sound's first sound is always one, at
// full strength, from 0.
struct Onset
{
    int sample = 0;
    float strength = 1.0f;
    float slicesFrom = 0.0f;
};

// The sound's fundamental as a note number with its fraction (69 is A at 440 Hz), from A0 to C8, where it holds steady
// past its attack (an 808 after its pitch has dropped), its channels mixed; none for a sound without a clear pitch, such as
// a snare or noise. Takes a few milliseconds.
std::optional<double> detectPitch (const juce::AudioBuffer<float>&, double sampleRate);

// Where its hits begin, in time order, at least 40 ms apart, each placed just before its rise and on a zero crossing.
std::vector<Onset> detectOnsets (const juce::AudioBuffer<float>&, double sampleRate);

// The zero crossing nearest a sample (the sample of the two around it that is nearer zero), within reach samples either
// way, its channels mixed; the sample itself where there is none, and within the sound always.
int findZeroCrossing (const juce::AudioBuffer<float>&, int sample, int reach);
} // namespace phazite::devices
