// SPDX-License-Identifier: AGPL-3.0-or-later
// The wavetable synth's oscillators (devices/wavetable/WavetableSynth.h): what each plays, for its editor, and choosing
// one of the factory tables or a sound file for one, each as an undo step; kept in its device's SOUNDS in the song
// (Schema.h: getOscillatorWavetable() and getOscillatorWavetableFile()): one of the factory tables by name, or a sound file
// in the project's folder, which is read into a table.
#pragma once

#include "devices/wavetable/WavetableSynth.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"

#include <memory>

namespace phazite
{
// Whether a device is Phazite's wavetable synth.
bool isWavetableSynth (const Song&, DeviceID);

// What an oscillator plays: its sound file's name, or its factory table's ("Basic Shapes"); whether it plays a file; and the
// table itself, for drawing: the one the engine plays, or else the one its sounds name (nullptr for another device).
juce::String getWavetableName (const Song&, DeviceID, int osc);
bool hasWavetableFile (const Song&, DeviceID, int osc);
std::shared_ptr<const devices::Wavetable> getWavetable (const Song&, DeviceID, int osc);

// Puts one of the factory tables (devices/wavetable/FactoryWavetables.h) on an oscillator, as one undo step named after it
// ("Put Vowels on Osc 1"); nothing if it plays that one already.
void setFactoryWavetable (Song&, DeviceID, int osc, int index);

// Puts a sound file on an oscillator as its table (devices/wavetable/Wavetable.h: fromSound()), as one undo step named after
// it: the project plays its own copy, in its Audio folder (model/arrangement/internal/Import.h). Returns false, changing
// nothing, for a file that isn't a sound Phazite can read.
bool setWavetableFile (Song&, DeviceID, int osc, const juce::File&);

// A sound file as a wavetable (devices/wavetable/Wavetable.h: fromSound()), its channels mixed, with the length of its frames
// from Serum's 'clm ' chunk if it has one; or nullptr if it can't be read.
std::shared_ptr<const devices::Wavetable> readWavetable (const juce::File&);

// Loads what an oscillator plays into a wavetable synth, as its device's SOUNDS in the song name it: its file, if it can be
// read, or else the factory table it names.
void loadOscillatorTable (devices::WavetableSynth&, const Song&, const juce::ValueTree& sounds, int osc);
} // namespace phazite
