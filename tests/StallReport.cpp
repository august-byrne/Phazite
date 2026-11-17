// SPDX-License-Identifier: AGPL-3.0-or-later
#include "StallReport.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#if JUCE_WINDOWS && defined(_M_X64)
 #define PHAZITE_STALL_STACKS 1

 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif

 // Windows' own headers, whose warnings aren't Phazite's.
 #pragma warning(push, 0)
 #include <windows.h>

 #include <dbghelp.h>
 #pragma warning(pop)

 #pragma comment(lib, "dbghelp.lib")
#endif

#if JUCE_LINUX
 #include <execinfo.h>
 #include <pthread.h>
 #include <signal.h>
 #include <unistd.h>
#endif

namespace phazite::test
{
namespace
{
    static_assert (std::atomic<std::int64_t>::is_always_lock_free);

    std::int64_t getNow()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now().time_since_epoch()).count();
    }

#if PHAZITE_STALL_STACKS
    constexpr int maxFrames = 48;

    // Where a thread is and what called it, innermost first, as addresses: the thread held still meanwhile. Nothing here
    // may allocate or take a lock the thread could hold (it may be in the middle of an allocation), so the frames are
    // unwound by the tables the linker made, and the names are found once it runs again. Not instrumented by
    // AddressSanitizer, which would check the reads of the other thread's context.
    __declspec (no_sanitize_address) int captureStack (HANDLE thread, DWORD64* addresses)
    {
        if (SuspendThread (thread) == (DWORD) -1)
            return 0;

        int count = 0;
        CONTEXT context;
        std::memset (&context, 0, sizeof (context));
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;

        if (GetThreadContext (thread, &context))
        {
            while (count < maxFrames && context.Rip != 0)
            {
                addresses[count++] = context.Rip;
                DWORD64 imageBase = 0;

                if (auto* function = RtlLookupFunctionEntry (context.Rip, &imageBase, nullptr))
                {
                    void* handlerData = nullptr;
                    DWORD64 establisherFrame = 0;
                    RtlVirtualUnwind (UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisherFrame,
                                      nullptr);
                }
                else
                {
                    // A function with no table is a leaf: what called it is at the top of its stack, if that can be read.
                    DWORD64 caller = 0;

                    if (! ReadProcessMemory (GetCurrentProcess(), reinterpret_cast<const void*> (context.Rsp), &caller, sizeof (caller),
                                             nullptr))
                        break;

                    context.Rip = caller;
                    context.Rsp += sizeof (caller);
                }
            }
        }

        ResumeThread (thread);
        return count;
    }

    // Each address as its module, its function and its line, as far as the program's symbols say.
    void writeStack (const DWORD64* addresses, int count)
    {
        const auto process = GetCurrentProcess();
        SymSetOptions (SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        SymInitialize (process, nullptr, TRUE); // fails where JUCE's own stack traces have done it already, which is as good

        for (int i = 0; i < count; ++i)
        {
            const auto address = addresses[i];

            IMAGEHLP_MODULE64 moduleInfo;
            std::memset (&moduleInfo, 0, sizeof (moduleInfo));
            moduleInfo.SizeOfStruct = sizeof (moduleInfo);
            const auto hasModule = SymGetModuleInfo64 (process, address, &moduleInfo) != FALSE;

            alignas (SYMBOL_INFO) char buffer[sizeof (SYMBOL_INFO) + 512];
            std::memset (buffer, 0, sizeof (buffer));
            auto* symbol = reinterpret_cast<SYMBOL_INFO*> (buffer);
            symbol->SizeOfStruct = sizeof (SYMBOL_INFO);
            symbol->MaxNameLen = 511;
            DWORD64 fromFunction = 0;
            const auto hasSymbol = SymFromAddr (process, address, &fromFunction, symbol) != FALSE;

            IMAGEHLP_LINE64 line;
            std::memset (&line, 0, sizeof (line));
            line.SizeOfStruct = sizeof (line);
            DWORD fromLine = 0;
            const auto hasLine = SymGetLineFromAddr64 (process, address, &fromLine, &line) != FALSE;

            std::fprintf (stderr, "    %2d: %s: ", i, hasModule ? moduleInfo.ModuleName : "?");

            if (hasSymbol)
                std::fprintf (stderr, "%s + 0x%llx", symbol->Name, (unsigned long long) fromFunction);
            else
                std::fprintf (stderr, "0x%llx", (unsigned long long) address);

            if (hasLine)
                std::fprintf (stderr, " (%s:%lu)", line.FileName, (unsigned long) line.LineNumber);

            std::fprintf (stderr, "\n");
        }
    }
#endif

#if JUCE_LINUX
    // On Linux the tests' thread writes its own stack, asked with a signal: its frames as glibc finds them, named as far
    // as the program exports its functions (JUCE's are; the rest are addresses). A test file that hung there said its
    // test case's name and nothing of where it was (AppTests, CI, 2026-11-17: known bug 190).
    void writeOwnStack (int)
    {
        void* frames[48];
        const auto count = backtrace (frames, 48);
        backtrace_symbols_fd (frames, count, STDERR_FILENO);
    }
#endif
} // namespace

struct StallReport::Watch
{
    explicit Watch (int secondsToAllow)
        : seconds (secondsToAllow)
    {
#if PHAZITE_STALL_STACKS
        testsThread = OpenThread (THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
#endif
#if JUCE_LINUX
        // What backtrace() loads at its first call, loaded now and not in the signal's handler; calls the signal
        // interrupts start again (a test case that is only slow goes on as it was).
        void* first[1];
        backtrace (first, 1);
        linuxTestsThread = pthread_self();
        struct sigaction action = {};
        action.sa_handler = writeOwnStack;
        action.sa_flags = SA_RESTART;
        sigemptyset (&action.sa_mask);
        sigaction (SIGUSR2, &action, nullptr);
#endif
        mark (nullptr, false);
        thread = std::thread ([this] { run(); });
    }

    ~Watch()
    {
        {
            const std::lock_guard lock (mutex);
            stopping = true;
        }

        wake.notify_all();
        thread.join();

#if PHAZITE_STALL_STACKS
        if (testsThread != nullptr)
            CloseHandle (testsThread);
#endif
    }

    // What the tests' thread does from now: a test case, or what comes after one (or before the first, with no name).
    void mark (const char* testCase, bool running)
    {
        name.store (testCase, std::memory_order_relaxed);
        isRunning.store (running, std::memory_order_relaxed);
        since.store (getNow(), std::memory_order_relaxed);
        changes.fetch_add (1, std::memory_order_release);
    }

    void run()
    {
        // So many reports of one test case: more than an hour of them with two minutes between.
        constexpr int maxReports = 40;
        int reported = 0;
        auto seen = changes.load (std::memory_order_acquire);
        std::unique_lock lock (mutex);

        while (! stopping)
        {
            wake.wait_for (lock, std::chrono::seconds (1));

            if (const auto now = changes.load (std::memory_order_acquire); now != seen)
            {
                seen = now;
                reported = 0;
            }

            const auto taken = (getNow() - since.load (std::memory_order_relaxed)) / 1000;

            if (stopping || reported >= maxReports || taken < (std::int64_t) seconds * (reported + 1))
                continue;

            ++reported;
            report ((long long) taken);
        }
    }

    void report (long long taken) const
    {
        const auto* testCase = name.load (std::memory_order_relaxed);

        if (testCase == nullptr)
            std::fprintf (stderr, "\nSTALLED: %lld s before the first test case.\n", taken);
        else if (isRunning.load (std::memory_order_relaxed))
            std::fprintf (stderr, "\nSTALLED: the test case \"%s\" has run for %lld s.\n", testCase, taken);
        else
            std::fprintf (stderr, "\nSTALLED: %lld s since the test case \"%s\" ended (between test cases, or as the tests end).\n", taken,
                          testCase);

#if PHAZITE_STALL_STACKS
        if (testsThread != nullptr)
        {
            DWORD64 addresses[maxFrames] = {};
            const auto count = captureStack (testsThread, addresses);
            std::fprintf (stderr, "  The tests' thread is at:\n");
            writeStack (addresses, count);
        }
#endif

#if JUCE_LINUX
        std::fprintf (stderr, "  The tests' thread is at:
");
        std::fflush (stderr);
        pthread_kill (linuxTestsThread, SIGUSR2);
        std::this_thread::sleep_for (std::chrono::milliseconds (500)); // its handler writes meanwhile
#endif

        std::fflush (stderr);
    }

    const int seconds;
    std::atomic<const char*> name { nullptr };
    std::atomic<bool> isRunning { false };
    std::atomic<std::int64_t> since { 0 };
    std::atomic<int> changes { 0 };

#if PHAZITE_STALL_STACKS
    HANDLE testsThread = nullptr;
#endif
#if JUCE_LINUX
    pthread_t linuxTestsThread {};
#endif

    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::thread thread;
};

int StallReport::getSecondsAsked()
{
    return juce::SystemStats::getEnvironmentVariable ("PHAZITE_TEST_STALL_SECONDS", {}).getIntValue();
}

StallReport::StallReport (int seconds)
    : watch (std::make_unique<Watch> (seconds))
{
}

StallReport::~StallReport() = default;

void StallReport::testCaseStarted (const char* name)
{
    watch->mark (name, true);
}

void StallReport::testCaseEnded()
{
    watch->mark (watch->name.load (std::memory_order_relaxed), false);
}
} // namespace phazite::test
