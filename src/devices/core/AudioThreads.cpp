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
    thread.startRealtimeThread (juce::Thread::RealtimeOptions {}.withPriority (10));
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
