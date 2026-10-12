// SPDX-License-Identifier: AGPL-3.0-or-later
// Shared includes for Phazite's tests.
#pragma once

#include <juce_events/juce_events.h>

// doctest, through Tracktion's wrapper, which silences doctest's warnings under JUCE's warning flags.
#include <tracktion_engine/../3rd_party/doctest/tracktion_doctest.hpp>

// doctest prints juce::Strings in failure messages.
namespace juce
{
inline doctest::String toString (const juce::String& s)
{
    return s.toRawUTF8();
}
} // namespace juce
