// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ProcessMemory.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// psapi.h needs windows.h first.
#include <psapi.h>
#else
#include <cstdio>
#include <unistd.h>
#endif

namespace phazite::test
{
std::int64_t getProcessMemoryBytes()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters {};
    counters.cb = sizeof (counters);

    // K32GetProcessMemoryInfo, in kernel32 itself: no psapi.lib to link.
    if (! K32GetProcessMemoryInfo (GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*> (&counters), sizeof (counters)))
        return 0;

    return (std::int64_t) counters.PrivateUsage;
#else
    long pages = 0, resident = 0;
    auto* file = std::fopen ("/proc/self/statm", "r");

    if (file == nullptr)
        return 0;

    const auto read = std::fscanf (file, "%ld %ld", &pages, &resident);
    std::fclose (file);
    return read == 2 ? (std::int64_t) resident * (std::int64_t) sysconf (_SC_PAGESIZE) : 0;
#endif
}
} // namespace phazite::test
