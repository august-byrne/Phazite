// SPDX-License-Identifier: AGPL-3.0-or-later
// How Phazite's own threads that play audio get the audio device's priority, never raising the whole process's: the
// engine's workers (engine/core/Workers.h) and the helpers' threads that play plug-ins (host/core/Instance.h). On
// Windows, the highest priority, and MMCSS's "Pro Audio" task joined from the thread itself, as JUCE's WASAPI thread
// does (JUCE's real-time threads put the whole process in the real-time priority class there, or the high one without
// an administrator's rights, every thread of the app above other programs'); elsewhere JUCE's real-time threads, or the
// highest priority where the system allows no real-time threads.
#pragma once

#include <juce_core/juce_core.h>

namespace phazite::devices
{
// Starts a thread that plays audio.
void startAudioThread (juce::Thread&);

// A moment's pause in a spin, which lets the core's other thread on.
void pauseInSpin() noexcept;

// An audio thread's wait for what another is about to finish (a task of the same block, which is real-time safe and ends
// by the block's deadline): spun for, a pause in each turn, never giving the core away. On Windows 11, an audio thread in
// MMCSS's Pro Audio task that yielded (std::this_thread::yield(), which is SwitchToThread() there, or Sleep (0)) while a
// thread at the normal priority was busy on its core lost the core for up to 16 ms, several blocks, in a fifth to a
// third of its waits; spinning, never more than microseconds.
template <typename Done>
void spinUntil (Done&& done) noexcept
{
    while (! done())
        pauseInSpin();
}

// While it lives, the thread that made it is in MMCSS's Pro Audio task (Windows; nothing elsewhere). Made first thing in
// the thread's run().
class ProAudioTask
{
public:
    ProAudioTask();
    ~ProAudioTask();

    ProAudioTask (const ProAudioTask&) = delete;
    ProAudioTask& operator= (const ProAudioTask&) = delete;

private:
#if JUCE_WINDOWS
    juce::DynamicLibrary avrt { "avrt.dll" }; // loaded as juce_WASAPI_windows.cpp loads it
    void* task = nullptr;
#endif
};
} // namespace phazite::devices
