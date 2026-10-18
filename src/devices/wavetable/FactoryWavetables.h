// SPDX-License-Identifier: AGPL-3.0-or-later
// The tables that come with Phazite's wavetable synth (roadmap step 2.13), made from formulas the first time each is
// needed, so that Phazite ships no sound files (docs/design.md, section 9): basic shapes, pulse widths, harmonics built up
// one by one, vowels, hard sync, FM, wavefolding, bit steps, a comb sweep, a talking bass and random spectra. Projects
// keep an oscillator's factory table by its name.
#pragma once

#include "Wavetable.h"

#include <memory>
#include <span>
#include <string_view>

namespace phazite::devices
{
// In the order menus list them; the first is what a new oscillator plays.
std::span<const std::string_view> getFactoryWavetableNames();

// The index of the factory table with that name, or -1.
int findFactoryWavetable (std::string_view name);

// A factory table, made once and then shared while anything holds it; for an index that isn't one, the first. Not on
// the audio thread: making one takes a few milliseconds.
std::shared_ptr<const Wavetable> getFactoryWavetable (int index);
} // namespace phazite::devices
