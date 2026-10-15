// SPDX-License-Identifier: AGPL-3.0-or-later
// The text that controls show, and screen readers read out, for common kinds of values.
#pragma once

#include "devices/core/DeviceInfo.h"

#include <juce_core/juce_core.h>

#include <optional>

namespace phazite::ui
{
// Levels at or below this many decibels are silence.
inline constexpr double silenceDb = -96.0;

// A level or gain, to a tenth of a decibel: "-6.0 dB", "+2.5 dB", "0.0 dB", or "-inf dB" for silence.
juce::String formatDecibels (double dB);

// Reads decibels as someone might type them ("-6", "-6 dB", "+3dB", "-inf"). Nothing if it isn't a level.
std::optional<double> parseDecibels (const juce::String&);

// A pan position from -1 (left) to 1 (right): "Center", "30% left", "100% right".
juce::String formatPan (double pan);

// Reads a pan position as formatPan() writes it, or as a number from -100 (left) to 100 (right).
std::optional<double> parsePan (const juce::String&);

// A value of one of Phazite's devices' parameters, as its unit says: "120 Hz", "1.20 kHz", "+3.0 dB", "35%", "12.5 ms",
// "1.20 s", "+7 st", "-15 ct", "4.0:1", a choice's name, "On" or "Off", or a plain number ("0.71").
juce::String formatParameterValue (const devices::ParameterInfo&, float value);

// Reads a value as formatParameterValue() writes it, or as someone might type it: a plain number in the unit, "1.2k"
// for 1.2 kHz, "250ms" for a quarter of a second, a choice's name or number, "on" or "off". Kept to the parameter's
// range; nothing if it can't be read.
std::optional<float> parseParameterValue (const devices::ParameterInfo&, const juce::String&);
} // namespace phazite::ui
