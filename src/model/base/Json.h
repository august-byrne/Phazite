// SPDX-License-Identifier: AGPL-3.0-or-later
// JSON read from text, wherever Phazite reads it: a preset or a keymap from others, a download, a setting, its own files.
#pragma once

#include <juce_core/juce_core.h>

#include <optional>

namespace phazite
{
// The value a JSON text holds, as juce::JSON::parse() reads it; void where the text isn't JSON, and where it has a number
// that JUCE's parser can't count: a whole number past the largest of 64 bits, or an exponent of ten digits or more. The
// parser counts a number's digits up in an integer before it knows whether they fit, and one that doesn't fit overflows
// it, which is undefined behaviour (the fuzzer's finding with many rounds under UndefinedBehaviorSanitizer, 2026-11-15).
// No number Phazite writes is so long, and a file with one is a damaged file. JUCE workaround W38
// (docs/juce-workarounds.md).
juce::var parseJson (const juce::String& text);

// The same, of a file's text; void for a file that can't be read too.
juce::var parseJson (const juce::File& file);

// Whether JUCE's reader of numbers in text would overflow as it reads this one, which juce::String::getDoubleValue() and so
// every number read from XML, a juce::var's text or a song's property goes through (CharacterFunctions::readDoubleValue()):
// a number whose exponent has ten digits or more past its leading zeros, which the reader counts into an int without a
// check. Only what the reader takes for a number's exponent: after a number's digits
// at the text's start ("1e1234567890"), not "Take e1234567890". JUCE workaround W38 (docs/juce-workarounds.md).
bool hasExponentTooLongToRead (const juce::String& text);

// That text as a number JUCE reads without overflowing, as JUCE's reader would take it were it to count the exponent: one
// past every double's ("1e999" or "-1e999", which read as infinity) for an exponent above 0, whatever its digits before it
// ("0e12345678901" too, which that reader takes for infinity), and 0 ("0" or "-0") for one below; what follows the number
// in the text stays. Any other text as it is.
juce::String makeExponentReadable (const juce::String& text);

// A text's number as juce::String::getDoubleValue() and getFloatValue() read it (0 for none), never one their reader would
// overflow (makeExponentReadable()), for text typed, or that a file or a plug-in gives; and `otherwise` in place of what is
// no finite number ("1e999", "inf", "nan"): neither ever returns one. A float is held within a float's range (a cast of a
// double past it is undefined behaviour, known bug 148). And the same as a number that is none for what isn't finite, for a
// value typed, where nothing should change.
double readDouble (const juce::String& text, double otherwise = 0.0);
float readFloat (const juce::String& text, float otherwise = 0.0f);
std::optional<double> readFiniteDouble (const juce::String& text);
} // namespace phazite
