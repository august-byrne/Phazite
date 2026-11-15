// SPDX-License-Identifier: AGPL-3.0-or-later
// JSON read from text, wherever Phazite reads it: a preset or a keymap from others, a download, a setting, its own files.
#pragma once

#include <juce_core/juce_core.h>

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
} // namespace phazite
