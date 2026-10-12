// SPDX-License-Identifier: AGPL-3.0-or-later
// Shared includes and helpers for Phazite's tests.
#pragma once

#include <juce_events/juce_events.h>
#include <tracktion_engine/tracktion_engine.h>

// doctest, through Tracktion's wrapper, which silences doctest's warnings under JUCE's warning flags.
#include <tracktion_engine/../3rd_party/doctest/tracktion_doctest.hpp>

namespace te = tracktion;

namespace phazite::test
{
// The Engine that all tests share, made by TestMain.cpp like the app's (no audio device, no plug-in
// scanning). Its settings live in a temporary folder, so tests never touch Phazite's real settings.
te::Engine& getEngine();

// The tests have no message loop, but some engine work happens on the message thread's timers (finding
// MIDI devices, for example). This runs the timers that are due, as the loop would, until the
// condition holds. Returns false if it still doesn't after timeoutMilliseconds.
bool runTimersUntil (const std::function<bool()>& condition, int timeoutMilliseconds = 2000);
} // namespace phazite::test

// doctest prints juce::Strings in failure messages.
namespace juce
{
inline doctest::String toString (const juce::String& s)
{
    return s.toRawUTF8();
}
} // namespace juce
