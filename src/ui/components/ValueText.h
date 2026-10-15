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

// A tempo, to a hundredth of a beat per minute, without zeros after the point: "120 BPM", "128.5 BPM", "87.25 BPM".
juce::String formatTempo (double bpm);

// Reads a tempo as someone might type it ("128", "128.5 bpm"). Nothing if it isn't a number.
std::optional<double> parseTempo (const juce::String&);

// A time signature: "4/4", "6/8".
juce::String formatTimeSignature (int numerator, int denominator);

// An integrated loudness, to a tenth: "-14.0 LUFS".
juce::String formatLoudness (double lufs);

// Reads a loudness as someone might type it ("-14", "-14 LUFS", "-23 LKFS"). Nothing if it isn't a number.
std::optional<double> parseLoudness (const juce::String&);

// A true peak, to a tenth: "-1.0 dBTP".
juce::String formatTruePeak (double dBTP);

// Reads a true peak as someone might type it ("-1", "-1 dBTP", "-1 dB"). Nothing if it isn't a number.
std::optional<double> parseTruePeak (const juce::String&);

// How long something lasts, in minutes and whole seconds: "0:04", "2:31", "61:05".
juce::String formatDuration (double seconds);

// A sample rate, in kilohertz: "44.1 kHz", "48 kHz".
juce::String formatSampleRate (double hz);

// What showing a file in its folder is called in the system's file manager: "Show in Explorer", "Show in Finder", or
// "Show in File Manager".
juce::String getShowInFolderText();

// A value of one of Phazite's devices' parameters, as its unit says: "120 Hz", "1.20 kHz", "+3.0 dB", "35%", "12.5 ms",
// "1.20 s", "+7 st", "-15 ct", "4.0:1", a choice's name, "On" or "Off", or a plain number ("0.71").
juce::String formatParameterValue (const devices::ParameterInfo&, float value);

// Reads a value as formatParameterValue() writes it, or as someone might type it: a plain number in the unit, "1.2k"
// for 1.2 kHz, "250ms" for a quarter of a second, a choice's name or number, "on" or "off". Kept to the parameter's
// range; nothing if it can't be read.
std::optional<float> parseParameterValue (const devices::ParameterInfo&, const juce::String&);
} // namespace phazite::ui
