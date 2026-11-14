// SPDX-License-Identifier: AGPL-3.0-or-later
// Counts heap allocations and frees on audio threads, for the real-time audit: threads marked as such (ScopedAudioThread:
// the tests' audio device's), and Phazite's own that play audio (devices/core/AudioThreads.h: the engine's workers). It
// measures, with each call's stack, in Windows Debug builds (the debug C runtime's allocation hook) and in Linux builds
// with glibc (the test program's own malloc and free, which hand each call on to glibc's); without stacks under Linux's
// AddressSanitizer and ThreadSanitizer (their allocator's hooks). Elsewhere (Windows Release builds, and windows-asan, whose
// allocator the debug hook doesn't see) isAvailable() is false and the counts stay at zero.
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

    // How many allocations and frees have happened since reset(): the number of the next one, for describeCallSites().
    static int getNumEvents();

    // Where the recorded events came from (the first 512 after reset(), from the one numbered firstEvent on), grouped by
    // call stack and most frequent first, e.g. "12 x  phazite::Foo::bar <- phazite::Baz::process <- ...".
    static juce::StringArray describeCallSites (int framesPerSite = 6, int firstEvent = 0);
};
} // namespace phazite::test
