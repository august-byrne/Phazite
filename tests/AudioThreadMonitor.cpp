// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AudioThreadMonitor.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <vector>

#if JUCE_WINDOWS && defined(_DEBUG)
 #define PHAZITE_ALLOCATION_HOOK 1
#else
 #define PHAZITE_ALLOCATION_HOOK 0
#endif

#if PHAZITE_ALLOCATION_HOOK
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <crtdbg.h>
 #include <windows.h>
 // dbghelp.h needs windows.h first.
 #include <dbghelp.h>
 #pragma comment(lib, "dbghelp.lib")
#endif

namespace phazite::test
{
namespace
{
    thread_local bool isAudioThread = false;
    std::atomic<int> numAllocations { 0 }, numFrees { 0 };

#if PHAZITE_ALLOCATION_HOOK
    constexpr int maxRecordedEvents = 512;
    constexpr int maxFrames = 24;

    struct RecordedEvent
    {
        void* frames[maxFrames];
        unsigned short numFrames;
    };

    RecordedEvent recordedEvents[maxRecordedEvents];
    std::atomic<int> numRecordedEvents { 0 };

    // Called by the debug C runtime for every malloc, realloc and free in the process, which includes
    // operator new and juce::HeapBlock. It must not allocate itself: it only counts and copies return
    // addresses into the fixed array above.
    int __cdecl allocationHook (int type, void*, size_t, int blockType, long, const unsigned char*, int)
    {
        if (! isAudioThread || blockType == _CRT_BLOCK) // _CRT_BLOCK: the C runtime's own bookkeeping
            return TRUE;

        if (type == _HOOK_FREE)
            ++numFrees;
        else
            ++numAllocations;

        if (const auto index = numRecordedEvents.fetch_add (1); index < maxRecordedEvents)
        {
            auto& event = recordedEvents[index];
            event.numFrames = RtlCaptureStackBackTrace (1, maxFrames, event.frames, nullptr);
        }

        return TRUE;
    }

    juce::String getSymbolName (HANDLE process, void* address)
    {
        alignas (SYMBOL_INFO) char storage[sizeof (SYMBOL_INFO) + MAX_SYM_NAME];
        auto* symbol = reinterpret_cast<SYMBOL_INFO*> (storage);
        symbol->SizeOfStruct = sizeof (SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;

        if (DWORD64 displacement = 0; SymFromAddr (process, reinterpret_cast<DWORD64> (address), &displacement, symbol))
            return juce::String (symbol->Name)
                .replace ("tracktion::engine::", "te::")
                .replace ("tracktion::graph::", "tg::")
                .replace ("tracktion::core::", "tc::");

        return juce::String::toHexString (reinterpret_cast<juce::pointer_sized_int> (address));
    }

    // Frames that only say *how* memory was allocated, not *who* asked for it.
    bool isAllocatorFrame (const juce::String& name)
    {
        for (auto* part : { "malloc", "realloc", "calloc", "free", "_dbg", "heap_alloc", "operator new", "operator delete",
                            "juce::HeapBlock", "juce::ArrayBase", "std::allocator", "std::_", "allocationHook" })
            if (name.contains (part))
                return true;

        return false;
    }
#endif
} // namespace

bool AudioThreadMonitor::isAvailable()
{
    return PHAZITE_ALLOCATION_HOOK != 0;
}

AudioThreadMonitor::ScopedAudioThread::ScopedAudioThread()
{
    isAudioThread = true;
}

AudioThreadMonitor::ScopedAudioThread::~ScopedAudioThread()
{
    isAudioThread = false;
}

void AudioThreadMonitor::reset()
{
#if PHAZITE_ALLOCATION_HOOK
    static const auto previousHook = _CrtSetAllocHook (allocationHook);
    juce::ignoreUnused (previousHook);
    numRecordedEvents = 0;
#endif
    numAllocations = 0;
    numFrees = 0;
}

AudioThreadMonitor::Counts AudioThreadMonitor::getCounts()
{
    return { numAllocations.load(), numFrees.load() };
}

int AudioThreadMonitor::getNumEvents()
{
    return numAllocations.load() + numFrees.load();
}

juce::StringArray AudioThreadMonitor::describeCallSites ([[maybe_unused]] int framesPerSite, [[maybe_unused]] int firstEvent)
{
    juce::StringArray lines;

#if PHAZITE_ALLOCATION_HOOK
    const auto process = GetCurrentProcess();
    SymSetOptions (SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);

    // JUCE's SystemStats::getStackBacktrace() initialises the symbol handler without cleaning it up, and a second
    // SymInitialize() then fails: the symbols are there to use, but aren't this function's to clean up.
    const auto initialised = SymInitialize (process, nullptr, TRUE) != FALSE;

    if (! initialised && GetLastError() != ERROR_INVALID_PARAMETER)
        return { "(symbols unavailable)" };

    std::map<juce::String, int> sites;

    for (int i = juce::jmax (0, firstEvent); i < juce::jmin (numRecordedEvents.load(), maxRecordedEvents); ++i)
    {
        juce::StringArray frames;
        const auto& event = recordedEvents[i];

        for (int f = 0; f < event.numFrames && frames.size() < framesPerSite; ++f)
        {
            auto name = getSymbolName (process, event.frames[f]);

            if (frames.isEmpty() && isAllocatorFrame (name))
                continue; // skip the allocator's own frames, up to whoever called it

            frames.add (name);
        }

        ++sites[frames.joinIntoString (" <- ")];
    }

    if (initialised)
        SymCleanup (process);

    std::vector<std::pair<int, juce::String>> sorted;

    for (const auto& [site, count] : sites)
        sorted.emplace_back (count, site);

    std::sort (sorted.begin(), sorted.end(), [] (const auto& a, const auto& b) { return a.first > b.first; });

    for (const auto& [count, site] : sorted)
        lines.add (juce::String (count) + " x  " + site);
#endif

    return lines;
}
} // namespace phazite::test
