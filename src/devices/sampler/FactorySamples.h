// SPDX-License-Identifier: AGPL-3.0-or-later
// The sounds that come with the sampler (docs/design.md, section 9), synthesized the first time each is needed, so that
// Phazite ships no sound files: first an 808 bass at C2 (drums/DrumSynthesis.h), what a new sampler plays, so that it plays
// a finished sound across the keys at once; then the sounds made from their spectra (dsp/SpectralLoop.h), each one seamless
// loop in stereo, to play looped across the keys: "Wide Saw", a saw as an ensemble plays it, every harmonic a band a few
// cents wide, its two sides unrelated; then the drum sampler's factory sounds (drums/FactoryDrums.h), by their names.
// Projects and presets keep a sampler's factory sound by its name.
#pragma once

#include "Sampler.h"

#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace phazite::devices
{
// Every factory sound's name ("808 Bass", "Wide Saw", "808 Kick"), in the order menus list them: a new sampler plays the
// first. And how many of them come before the drum sampler's: the sounds made to play across the keys.
std::span<const std::string_view> getFactorySampleNames();
int getNumPitchedFactorySamples();

// The index of the factory sound with that name, or -1.
int findFactorySample (std::string_view name);

// A factory sound, with where its hits begin, its peak at -6 dB (a sound made from its spectrum at an RMS of -18 dB, which
// leaves its peaks near there, wherever its phases put them): made once, and then shared while anything holds it (the
// first for good, since every new sampler plays it); for an index that isn't one, the first. Not on the audio thread:
// making one takes a few milliseconds, one made from its spectrum some tens.
std::shared_ptr<const Sampler::Sound> getFactorySample (int index);

// The note that plays a factory sound at its own pitch: the 808 bass's C2, the note a sound made from its spectrum was made
// at, a drum's pitch where it has a clear one, or else middle C.
int getFactorySampleRoot (int index);

// The loop a factory sound is made to play in, as parts of its length: the whole of a sound made from its spectrum, which
// is one seamless loop; none for the others.
std::optional<std::pair<float, float>> getFactorySampleLoop (int index);
} // namespace phazite::devices
