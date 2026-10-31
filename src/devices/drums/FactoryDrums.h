// SPDX-License-Identifier: AGPL-3.0-or-later
// The sounds that come with Phazite's drum sampler (docs/design.md, section 9), synthesized the first time each is needed
// (DrumSynthesis.h), so that Phazite ships no sound files. The first sixteen are the default kit, in the pads' order: what
// each pad of a new sampler plays. Then come the kits of Phazite's drum presets (resources/presets/Drum Sampler/Kits),
// each sixteen sounds of its own in the same order: an 808, a techno kit after the 909, a house kit, a lo-fi kit as an old
// sampler plays it back, a punchy kit, a tight one, and a wide one in stereo. Projects and presets keep a pad's factory
// sound by its name.
#pragma once

#include "DrumSampler.h"

#include <memory>
#include <span>
#include <string_view>

namespace phazite::devices
{
// Every factory sound's name ("808 Kick"), in the order menus list them: a pad's default is the one at its index.
std::span<const std::string_view> getFactoryDrumNames();

// The index of the factory sound with that name, or -1.
int findFactoryDrum (std::string_view name);

// A factory sound, its peak at -6 dB so that hits add up: made once, and then shared while anything holds it (the default
// kit's for good, since every new sampler plays it); for an index that isn't one, the first. Not on the audio thread:
// making one takes a few milliseconds.
std::shared_ptr<const DrumSampler::Sound> getFactoryDrum (int index);
} // namespace phazite::devices
