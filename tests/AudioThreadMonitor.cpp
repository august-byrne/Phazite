// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AudioThreadMonitor.h"
#include "devices/core/AudioThreads.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <vector>

// How allocations are seen (AudioThreadMonitor.h):
// - Windows Debug builds: the debug C runtime's allocation hook, with each call's stack;
// - Linux builds with glibc and no sanitizer: the test program's own malloc and free, which stand in for the C library's
//   (an executable's definitions come first for every library it loads) and hand each call on to it, with each call's stack;
// - Linux builds with AddressSanitizer or ThreadSanitizer, whose allocator stands in for the C library's: its hooks, which
//   count without stacks;
// - elsewhere (Windows Release builds, Windows with AddressSanitizer, whose allocator the debug hook doesn't see), nothing.
#if defined(__has_feature)
 #if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
  #define PHAZITE_SANITIZER_BUILD 1
 #endif
#endif
#if ! defined(PHAZITE_SANITIZER_BUILD) && (defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__))
 #define PHAZITE_SANITIZER_BUILD 1
#endif
#ifndef PHAZITE_SANITIZER_BUILD
 #define PHAZITE_SANITIZER_BUILD 0
#endif

#if JUCE_WINDOWS && defined(_DEBUG) && ! PHAZITE_SANITIZER_BUILD
 #define PHAZITE_ALLOCATION_HOOK 1
#else
 #define PHAZITE_ALLOCATION_HOOK 0
#endif

#if JUCE_LINUX && defined(__GLIBC__) && ! PHAZITE_SANITIZER_BUILD
 #define PHAZITE_GLIBC_MALLOC 1
#else
 #define PHAZITE_GLIBC_MALLOC 0
#endif

#if JUCE_LINUX && PHAZITE_SANITIZER_BUILD
 #define PHAZITE_SANITIZER_HOOKS 1
#else
 #define PHAZITE_SANITIZER_HOOKS 0
#endif

#define PHAZITE_RECORDS_STACKS (PHAZITE_ALLOCATION_HOOK || PHAZITE_GLIBC_MALLOC)

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

#if PHAZITE_GLIBC_MALLOC
 #include <cerrno>
 #include <cstdlib>
 #include <cxxabi.h>
 #include <execinfo.h>
#endif

#if PHAZITE_SANITIZER_HOOKS
// The sanitizers' hooks for malloc and free, declared as their <sanitizer/allocator_interface.h> does: GCC's packages
// have the function in the sanitizers' libraries, but leave that header out.
extern "C" int __sanitizer_install_malloc_and_free_hooks (void (*mallocHook) (const volatile void*, size_t),
                                                          void (*freeHook) (const volatile void*));
#endif

namespace phazite::test
{
namespace
{
    thread_local bool isMarked = false;   // ScopedAudioThread's
    thread_local int inAudioBlocks = 0;   // ScopedAudioBlock's, which may nest
    std::atomic<bool> counting { false }; // from the first reset() on
    std::atomic<int> numAllocations { 0 }, numFrees { 0 };

#if PHAZITE_ALLOCATION_HOOK || PHAZITE_GLIBC_MALLOC || PHAZITE_SANITIZER_HOOKS
    // An audio thread: one the test marked (the audio device's), or one of Phazite's own (the engine's workers).
    bool isCountedThread() noexcept
    {
        return isMarked || devices::isOwnAudioThread();
    }
#endif

#if PHAZITE_RECORDS_STACKS
    constexpr int maxRecordedEvents = 512;
    constexpr int maxFrames = 24;

    struct RecordedEvent
    {
        void* frames[maxFrames];
        unsigned short numFrames;
        bool isFree;
    };

    RecordedEvent recordedEvents[maxRecordedEvents];
    std::atomic<int> numRecordedEvents { 0 };
#endif

#if PHAZITE_ALLOCATION_HOOK
    // Called by the debug C runtime for every malloc, realloc and free in the process, which includes
    // operator new and juce::HeapBlock. It must not allocate itself: it only counts and copies return
    // addresses into the fixed array above.
    int __cdecl allocationHook (int type, void*, size_t, int blockType, long, const unsigned char*, int)
    {
        if (! isCountedThread() || blockType == _CRT_BLOCK) // _CRT_BLOCK: the C runtime's own bookkeeping
            return TRUE;

        if (type == _HOOK_FREE)
            ++numFrees;
        else
            ++numAllocations;

        if (const auto index = numRecordedEvents.fetch_add (1); index < maxRecordedEvents)
        {
            auto& event = recordedEvents[index];
            event.numFrames = RtlCaptureStackBackTrace (1, maxFrames, event.frames, nullptr);
            event.isFree = type == _HOOK_FREE;
        }

        return TRUE;
    }

    // Sets up the symbol handler once, and keeps it to the end of the run: the first name looked up after setting it up
    // loads the test program's symbols, which in a Debug build takes seconds. JUCE's SystemStats::getStackBacktrace()
    // sets it up the same way, and SymInitialize() fails if it already has: the symbols are there to use all the same.
    bool hasSymbols (HANDLE process)
    {
        static const bool available = [process]
        {
            SymSetOptions (SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            return SymInitialize (process, nullptr, TRUE) != FALSE || GetLastError() == ERROR_INVALID_PARAMETER;
        }();

        return available;
    }

    juce::String getSymbolName (void* address)
    {
        const auto process = GetCurrentProcess();
        alignas (SYMBOL_INFO) char storage[sizeof (SYMBOL_INFO) + MAX_SYM_NAME];
        auto* symbol = reinterpret_cast<SYMBOL_INFO*> (storage);
        symbol->SizeOfStruct = sizeof (SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;

        if (DWORD64 displacement = 0; SymFromAddr (process, reinterpret_cast<DWORD64> (address), &displacement, symbol))
            return juce::String (symbol->Name);

        return juce::String::toHexString (reinterpret_cast<juce::pointer_sized_int> (address));
    }

    bool canNameSymbols()
    {
        return hasSymbols (GetCurrentProcess());
    }
#endif

#if PHAZITE_GLIBC_MALLOC
    // In a call of malloc or free that counts (backtrace() can allocate, the first time it loads the unwinder).
    thread_local bool inHook = false;

    void noteAllocation (bool isFree) noexcept
    {
        if (inHook || ! counting.load (std::memory_order_relaxed) || ! isCountedThread())
            return;

        inHook = true;

        if (isFree)
            ++numFrees;
        else
            ++numAllocations;

        if (const auto index = numRecordedEvents.fetch_add (1); index < maxRecordedEvents)
        {
            auto& event = recordedEvents[index];
            event.numFrames = (unsigned short) std::max (0, backtrace (event.frames, maxFrames));
            event.isFree = isFree;
        }

        inHook = false;
    }

    // The name of the function at an address, as backtrace_symbols() gives it ("program(name+0x12) [0x...]"; the test
    // program exports its symbols for it: CMakeLists.txt), demangled.
    juce::String getSymbolName (void* address)
    {
        char** names = backtrace_symbols (&address, 1);

        if (names == nullptr)
            return juce::String::toHexString (reinterpret_cast<juce::pointer_sized_int> (address));

        const juce::String line (names[0]);
        std::free (names);
        auto name = line.fromFirstOccurrenceOf ("(", false, false).upToFirstOccurrenceOf ("+", false, false);

        if (name.isEmpty())
            return line;

        int status = 0;

        if (char* demangled = abi::__cxa_demangle (name.toRawUTF8(), nullptr, nullptr, &status); demangled != nullptr)
        {
            name = juce::String (demangled);
            std::free (demangled);
        }

        return name;
    }

    bool canNameSymbols()
    {
        return true;
    }
#endif

#if PHAZITE_SANITIZER_HOOKS
    void sanitizerMallocHook (const volatile void*, size_t)
    {
        if (counting.load (std::memory_order_relaxed) && isCountedThread())
            ++numAllocations;
    }

    void sanitizerFreeHook (const volatile void*)
    {
        if (counting.load (std::memory_order_relaxed) && isCountedThread())
            ++numFrees;
    }
#endif

#if PHAZITE_RECORDS_STACKS
    // Frames that only say *how* memory was allocated, not *who* asked for it.
    bool isAllocatorFrame (const juce::String& name)
    {
        for (auto* part :
             { "malloc", "realloc", "calloc", "free", "_dbg", "heap_alloc", "operator new", "operator delete", "juce::HeapBlock",
               "juce::ArrayBase", "std::allocator", "std::_", "allocationHook", "noteAllocation", "__gnu_cxx::new_allocator", "backtrace" })
            if (name.contains (part))
                return true;

        return false;
    }

    // Names of functions by their addresses, each looked up once while one lives.
    struct SymbolNames
    {
        const juce::String& get (void* address)
        {
            auto found = names.find (address);

            if (found == names.end())
                found = names.emplace (address, getSymbolName (address)).first;

            return found->second;
        }

        std::map<void*, juce::String> names;
    };

    // Whether an event was JUCE's, making an assertion's message (juce_Logger.cpp: logAssertion()) or, on Linux, asking
    // whether a debugger is there to stop in (jassert's juce_isRunningUnderDebugger(), which reads /proc/self/status into
    // strings each time: 129 allocations an assertion, CI's linux-debug, 2026-11-17).
    bool isOfAssertion (const RecordedEvent& event, SymbolNames& names)
    {
        for (int f = 0; f < event.numFrames; ++f)
            if (const auto name = names.get (event.frames[f]);
                name.contains ("juce::logAssertion") || name.contains ("juce_isRunningUnderDebugger"))
                return true;

        return false;
    }
#endif
} // namespace

bool AudioThreadMonitor::isAvailable()
{
    return PHAZITE_ALLOCATION_HOOK != 0 || PHAZITE_GLIBC_MALLOC != 0 || PHAZITE_SANITIZER_HOOKS != 0;
}

bool AudioThreadMonitor::keepsStacks()
{
    return PHAZITE_RECORDS_STACKS != 0;
}

AudioThreadMonitor::ScopedAudioThread::ScopedAudioThread()
{
    isMarked = true;
}

AudioThreadMonitor::ScopedAudioThread::~ScopedAudioThread()
{
    isMarked = false;
}

AudioThreadMonitor::ScopedAudioBlock::ScopedAudioBlock() noexcept
{
    ++inAudioBlocks;
}

AudioThreadMonitor::ScopedAudioBlock::~ScopedAudioBlock()
{
    --inAudioBlocks;
}

bool AudioThreadMonitor::isAudioThread() noexcept
{
    return isMarked || inAudioBlocks > 0 || devices::isOwnAudioThread();
}

void AudioThreadMonitor::reset()
{
#if PHAZITE_ALLOCATION_HOOK
    static const auto previousHook = _CrtSetAllocHook (allocationHook);
    juce::ignoreUnused (previousHook);
#endif
#if PHAZITE_GLIBC_MALLOC
    // backtrace() loads the unwinder the first time, which allocates: here, not in a counted call.
    static const bool unwinderLoaded = []
    {
        void* frames[2];
        return backtrace (frames, 2) > 0;
    }();
    juce::ignoreUnused (unwinderLoaded);
#endif
#if PHAZITE_SANITIZER_HOOKS
    static const auto installed = __sanitizer_install_malloc_and_free_hooks (sanitizerMallocHook, sanitizerFreeHook);
    juce::ignoreUnused (installed);
#endif
#if PHAZITE_RECORDS_STACKS
    numRecordedEvents = 0;
#endif
    numAllocations = 0;
    numFrees = 0;
    counting = true;
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

#if PHAZITE_RECORDS_STACKS
    if (! canNameSymbols())
        return { "(symbols unavailable)" };

    std::map<juce::String, int> sites;
    SymbolNames names;

    for (int i = juce::jmax (0, firstEvent); i < juce::jmin (numRecordedEvents.load(), maxRecordedEvents); ++i)
    {
        juce::StringArray frames;
        const auto& event = recordedEvents[i];

        if (isOfAssertion (event, names))
            continue;

        for (int f = 0; f < event.numFrames && frames.size() < framesPerSite; ++f)
        {
            const auto& name = names.get (event.frames[f]);

            if (frames.isEmpty() && isAllocatorFrame (name))
                continue; // skip the allocator's own frames, up to whoever called it

            frames.add (name);
        }

        ++sites[frames.joinIntoString (" <- ")];
    }

    std::vector<std::pair<int, juce::String>> sorted;

    for (const auto& [site, count] : sites)
        sorted.emplace_back (count, site);

    std::sort (sorted.begin(), sorted.end(), [] (const auto& a, const auto& b) { return a.first > b.first; });

    for (const auto& [count, site] : sorted)
        lines.add (juce::String (count) + " x  " + site);
#endif

    return lines;
}

AudioThreadMonitor::Counts AudioThreadMonitor::ofAssertions ([[maybe_unused]] int firstEvent)
{
    Counts counts;

#if PHAZITE_RECORDS_STACKS
    if (! canNameSymbols())
        return counts;

    SymbolNames names;

    for (int i = juce::jmax (0, firstEvent); i < juce::jmin (numRecordedEvents.load(), maxRecordedEvents); ++i)
        if (const auto& event = recordedEvents[i]; isOfAssertion (event, names))
            ++(event.isFree ? counts.frees : counts.allocations);
#endif

    return counts;
}
} // namespace phazite::test

#if PHAZITE_GLIBC_MALLOC
// The test program's malloc and free, which every library it loads calls in place of the C library's (ELF interposition, as
// a replacement allocator's are), each handing the call on to glibc's own (its __libc_ names, which glibc exports for this),
// so that every block is glibc's whichever of them made or frees it. Those not defined here (valloc, reallocarray...) go to
// glibc's straight, uncounted: Phazite's code and JUCE's don't use them.
extern "C"
{
    void* __libc_malloc (size_t);
    void* __libc_calloc (size_t, size_t);
    void* __libc_realloc (void*, size_t);
    void* __libc_memalign (size_t, size_t);
    void __libc_free (void*);

    void* malloc (size_t size) noexcept
    {
        phazite::test::noteAllocation (false);
        return __libc_malloc (size);
    }

    void* calloc (size_t count, size_t size) noexcept
    {
        phazite::test::noteAllocation (false);
        return __libc_calloc (count, size);
    }

    void* realloc (void* block, size_t size) noexcept
    {
        phazite::test::noteAllocation (false);
        return __libc_realloc (block, size);
    }

    void* memalign (size_t, size_t) noexcept; // glibc's, which only its <malloc.h> declares

    void* memalign (size_t alignment, size_t size) noexcept
    {
        phazite::test::noteAllocation (false);
        return __libc_memalign (alignment, size);
    }

    void* aligned_alloc (size_t alignment, size_t size) noexcept
    {
        phazite::test::noteAllocation (false);
        return __libc_memalign (alignment, size);
    }

    int posix_memalign (void** block, size_t alignment, size_t size) noexcept
    {
        if (alignment == 0 || alignment % sizeof (void*) != 0 || (alignment & (alignment - 1)) != 0)
            return EINVAL;

        phazite::test::noteAllocation (false);
        auto* made = __libc_memalign (alignment, size);

        if (made == nullptr && size != 0)
            return ENOMEM;

        *block = made;
        return 0;
    }

    void free (void* block) noexcept
    {
        if (block != nullptr)
            phazite::test::noteAllocation (true);

        __libc_free (block);
    }
}
#endif
