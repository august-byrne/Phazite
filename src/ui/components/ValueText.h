// SPDX-License-Identifier: AGPL-3.0-or-later
// The text that controls show, and screen readers read out, for common kinds of values.
#pragma once

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
} // namespace phazite::ui
