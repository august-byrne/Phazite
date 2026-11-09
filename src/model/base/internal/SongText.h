// SPDX-License-Identifier: AGPL-3.0-or-later
// How Phazite writes a song as text, the same in a project file (model/project/internal/Project.h) and in a version's
// pieces (model/versions/internal/SongPieces.h), so that the same song always writes the same text (docs/project-format.md):
// each value in one way, bytes in standard base 64, and elements as XML that any XML reader takes. Plain functions, for any
// thread.
#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
// A value's text. A string as it is, without the characters XML 1.0 can't hold (the controls below 32 but tab, line feed
// and carriage return); true and false as 1 and 0; a whole number below 10^15 in its digits, without ".0", whatever type
// holds it; a float's value in the fewest digits that read back as that float ("0.3", not "0.300000011920929"); and any
// other number in 15 significant digits, as C's printf writes "%.15g" ("0.3" for 0.1 + 0.2), so that what arithmetic
// leaves past them is no change. std::to_chars writes both, the same on every computer.
juce::String writeValue (const juce::var&);

// Whether two values write the same text: a number read from a file as text, and the same number set by the model, are the
// same value, and so are two numbers that differ only past what is written.
bool isSameValue (const juce::var&, const juce::var&);

// Bytes as text, in base 64 as RFC 4648 has it (with "=" to fill the last group); and back, nothing for text that isn't
// base 64 of some bytes.
juce::String writeBytes (const juce::MemoryBlock&);
std::optional<juce::MemoryBlock> readBytes (const juce::String&);

// A value as an XML attribute holds it: & < > and " as entities, a tab and the line ends as character references (so that
// they come back), the characters XML can't hold left out, and everything else as it is, in UTF-8.
void writeEscaped (juce::OutputStream&, const juce::String&);

// An element's properties as text, in the order of their names (the bytes of their UTF-8, which for Phazite's names is
// their alphabetical order).
using PropertyTexts = std::vector<std::pair<juce::String, juce::String>>;
PropertyTexts getPropertyTexts (const juce::ValueTree&);
void sortByName (PropertyTexts&);

// An element's start tag, "<TYPE" with its properties, as a version's piece writes it: on one line, a space before each
// property. The caller ends it ("/>" or ">").
void writeStartTag (juce::OutputStream&, const juce::String& type, const PropertyTexts& sortedProperties);

// An element and everything in it as a project file holds it: an XML declaration, then each element on a line of its own,
// indented two spaces a level; each property on a line of its own, the first beside the element's type and the others
// under it; lines ending in a line feed.
void writeProjectText (juce::OutputStream&, const juce::ValueTree&);
} // namespace phazite
