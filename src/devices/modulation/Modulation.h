// SPDX-License-Identifier: AGPL-3.0-or-later
// Modulation as Phazite's devices take it (docs/design.md, section 10; roadmap A5, part 4), engine-neutral: an LFO's
// value at any point of its cycle, so that a device can follow it between the blocks the engine moves it in, and how
// modulation moves a parameter, as the engine's modulators (Modulators.h) move every parameter alike.
//
// A parameter is moved by one sum for the whole device. The same function, given a sum for each voice, is how an
// instrument will move a parameter for each of its notes, as CLAP's polyphonic modulation does.
#pragma once

#include "devices/core/DeviceInfo.h"

namespace phazite::devices
{
// An LFO's waves, in the order of its Wave parameter.
enum class LfoWave
{
    sine,
    triangle,
    sawUp,
    sawDown,
    square,
    fourStepsUp,
    fourStepsDown,
    eightStepsUp,
    eightStepsDown,
    random, // a new random value each cycle
    noise   // from one random value to the next over each cycle
};

// Whether a wave is set by the point of its cycle alone: random values and noise follow the modulator's own random
// numbers instead.
bool isPeriodic (LfoWave);

// A periodic wave at a point of its cycle (0 to 1), from 0 to 1.
float getWaveValue (LfoWave, float phase);

// What makes an LFO's value of its wave's.
struct LfoShape
{
    LfoWave wave = LfoWave::sine;
    float depth = 1.0f, offset = 0.0f;
    bool bipolar = false; // from -1 to 1 rather than from 0 to 1
};

// The LFO's value at a point of its cycle: its wave there, scaled by its depth, moved by its offset, and from -1 to 1 if
// it is bipolar.
float getLfoValue (const LfoShape&, float phase);

// A point of a cycle moved on by part of a cycle, wrapped to 0 to 1.
float movePhase (float phase, double cycles);

// A parameter's value moved by modulation: offset is the sum of each modulator's value times the amount it modulates
// the parameter by, as a part of the parameter's range as its control spans it (range, its getRange()). The result is
// kept to the range and to the parameter's steps.
float modulate (const ParameterInfo&, const juce::NormalisableRange<float>& range, float value, float offset);
} // namespace phazite::devices
