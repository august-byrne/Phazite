// SPDX-License-Identifier: AGPL-3.0-or-later
// The sounds that come with the sampler (docs/design.md, section 9), synthesized the first time each is needed
// (drums/DrumSynthesis.h), so that Phazite ships no sound files: first an 808 bass at C2, what a new sampler plays, so that
// it plays a finished sound across the keys at once; then the drum sampler's factory sounds (drums/FactoryDrums.h), by their
// names. Projects and presets keep a sampler's factory sound by its name.
#pragma once

#include "Sampler.h"

#include <memory>
#include <span>
#include <string_view>

namespace phazite::devices
{
// Every factory sound's name ("808 Bass", "808 Kick"), in the order menus list them: a new sampler plays the first.
std::span<const std::string_view> getFactorySampleNames();

// The index of the factory sound with that name, or -1.
int findFactorySample (std::string_view name);

// A factory sound, with where its hits begin, its peak at -6 dB: made once, and then shared while anything holds it (the
// first for good, since every new sampler plays it); for an index that isn't one, the first. Not on the audio thread:
// making one takes a few milliseconds.
std::shared_ptr<const Sampler::Sound> getFactorySample (int index);

// The note that plays a factory sound at its own pitch: the 808 bass's C2, a drum's pitch where it has a clear one, or else
// middle C.
int getFactorySampleRoot (int index);
} // namespace phazite::devices
