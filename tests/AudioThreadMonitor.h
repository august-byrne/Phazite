// SPDX-License-Identifier: AGPL-3.0-or-later
// Counts heap allocations and frees on threads marked as audio threads, for the real-time audit.
// It needs the debug C runtime's allocation hook, so it only measures in Windows Debug builds (the
// windows-debug preset); elsewhere isAvailable() is false and the counts stay at zero.
#pragma once

#include <juce_core/juce_core.h>

namespace phazite::test
{
class AudioThreadMonitor
{
public:
    static bool isAvailable();

    // Marks the calling thread as an audio thread while this object exists.
    struct ScopedAudioThread
    {
        ScopedAudioThread();
        ~ScopedAudioThread();
    };

    struct Counts
    {
        int allocations = 0; // including reallocations
        int frees = 0;
    };

    // Starts counting from zero. Call while no audio thread is running.
    static void reset();
    static Counts getCounts();

    // Where the recorded events came from (the first 512 after reset()), grouped by call stack and
    // most frequent first, e.g. "12 x  te::Foo::bar <- te::Baz::process <- ...".
    static juce::StringArray describeCallSites (int framesPerSite = 6);
};
} // namespace phazite::test
