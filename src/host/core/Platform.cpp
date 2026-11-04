// SPDX-License-Identifier: AGPL-3.0-or-later
// What the helper process asks of the operating system (Host.h): to end quietly when a plug-in crashes it, and its CPU
// time and memory, which the sandbox's benchmarks read (MessageType::stats).
#include "Host.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off: psapi.h needs windows.h first
#include <windows.h>
#include <psapi.h>
// clang-format on
#else
#include <fstream>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace phazite::host
{
void endQuietlyOnCrashes()
{
#if defined(_WIN32)
    // No dialog of Windows' for a crash (a plug-in's), which would keep the helper waiting for a click: it ends, and
    // Phazite says so in the plug-in's slot.
    SetErrorMode (SetErrorMode (0) | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
}

double getProcessCpuSeconds()
{
#if defined(_WIN32)
    FILETIME created, exited, kernel, user;

    if (! GetProcessTimes (GetCurrentProcess(), &created, &exited, &kernel, &user))
        return 0.0;

    const auto toSeconds = [] (const FILETIME& time)
    { return (double) ((static_cast<std::uint64_t> (time.dwHighDateTime) << 32) | time.dwLowDateTime) * 1.0e-7; };
    return toSeconds (kernel) + toSeconds (user);
#else
    rusage usage {};
    getrusage (RUSAGE_SELF, &usage);
    return (double) usage.ru_utime.tv_sec + usage.ru_utime.tv_usec * 1.0e-6 + (double) usage.ru_stime.tv_sec
           + usage.ru_stime.tv_usec * 1.0e-6;
#endif
}

std::int64_t getProcessMemoryBytes()
{
#if defined(_WIN32)
    // Its private working set: what of it is in memory and no other process's. (Its private bytes are much more: OLE,
    // which JUCE starts on Windows, commits tens of megabytes it never touches.) Without Windows' newer counters, its
    // private bytes.
    PROCESS_MEMORY_COUNTERS_EX2 counters {};
    counters.cb = sizeof (counters);

    if (GetProcessMemoryInfo (GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*> (&counters), sizeof (counters)))
        return (std::int64_t) counters.PrivateWorkingSetSize;

    PROCESS_MEMORY_COUNTERS_EX older {};
    older.cb = sizeof (older);

    if (! GetProcessMemoryInfo (GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*> (&older), sizeof (older)))
        return 0;

    return (std::int64_t) older.PrivateUsage;
#else
    // Resident pages less those shared with other processes (the program's code, libraries).
    std::ifstream statm ("/proc/self/statm");
    long size = 0, resident = 0, shared = 0;
    statm >> size >> resident >> shared;
    return (std::int64_t) (resident - shared) * (std::int64_t) sysconf (_SC_PAGESIZE);
#endif
}
} // namespace phazite::host
