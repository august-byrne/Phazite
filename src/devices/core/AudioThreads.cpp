// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AudioThreads.h"

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace phazite::devices
{
void pauseInSpin() noexcept
{
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__ ("yield");
#endif
}

#if JUCE_WINDOWS
namespace
{
    using Join = void*(__stdcall*) (const wchar_t*, unsigned long*);
    using Leave = int (__stdcall*) (void*);
} // namespace
#endif

void startAudioThread (juce::Thread& thread)
{
#if JUCE_WINDOWS
    thread.startThread (juce::Thread::Priority::highest);
#else
    // A real-time thread where the system allows it; where it doesn't (Linux without real-time rights, as a user not in an
    // audio group, or CI's runners), JUCE starts none at all, so a thread at the highest priority it allows.
    if (! thread.startRealtimeThread (juce::Thread::RealtimeOptions {}.withPriority (10)))
        thread.startThread (juce::Thread::Priority::highest);
#endif
}

ProAudioTask::ProAudioTask()
{
#if JUCE_WINDOWS
    if (auto* join = reinterpret_cast<Join> (avrt.getFunction ("AvSetMmThreadCharacteristicsW")))
    {
        unsigned long index = 0;
        task = join (L"Pro Audio", &index);
    }
#endif
}

ProAudioTask::~ProAudioTask()
{
#if JUCE_WINDOWS
    if (task != nullptr)
        if (auto* leave = reinterpret_cast<Leave> (avrt.getFunction ("AvRevertMmThreadCharacteristics")))
            leave (task);
#endif
}
} // namespace phazite::devices
