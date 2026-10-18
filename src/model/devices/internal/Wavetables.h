// SPDX-License-Identifier: AGPL-3.0-or-later
// The wavetable synth's oscillators (devices/wavetable/WavetableSynth.h): what each plays, for its editor, and choosing
// one of the factory tables or a sound file for one, each as an undo step.
#pragma once

#include "devices/wavetable/Wavetable.h"
#include "model/base/internal/Tracktion.h"

#include <memory>

namespace phazite
{
// Whether a device is Phazite's wavetable synth.
bool isWavetableSynth (const te::Plugin&);

// What an oscillator plays: its sound file's name, or its factory table's ("Basic Shapes"); whether it plays a file; and the
// table itself, for drawing (nullptr for another device).
juce::String getWavetableName (te::Plugin&, int osc);
bool hasWavetableFile (te::Plugin&, int osc);
std::shared_ptr<const devices::Wavetable> getWavetable (te::Plugin&, int osc);

// Puts one of the factory tables (devices/wavetable/FactoryWavetables.h) on an oscillator, as one undo step named after it
// ("Put Vowels on Osc 1"); nothing if it plays that one already.
void setFactoryWavetable (te::Plugin&, int osc, int index);

// Puts a sound file on an oscillator as its table (devices/wavetable/Wavetable.h: fromSound()), as one undo step named after
// it: the project plays its own copy, in its Audio folder (model/arrangement/internal/Import.h). Returns false, changing
// nothing, for a file that isn't a sound Phazite can read.
bool setWavetableFile (te::Plugin&, int osc, const juce::File&);
} // namespace phazite
