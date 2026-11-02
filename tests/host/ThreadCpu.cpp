// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ThreadCpu.h"

#include <cstdint>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <ctime>
#endif

namespace phazite::test
{
double getThreadCpuSeconds()
{
#if defined(_WIN32)
    FILETIME created, exited, kernel, user;

    if (! GetThreadTimes (GetCurrentThread(), &created, &exited, &kernel, &user))
        return 0.0;

    const auto toSeconds = [] (const FILETIME& time)
    { return (double) ((static_cast<std::uint64_t> (time.dwHighDateTime) << 32) | time.dwLowDateTime) * 1.0e-7; };
    return toSeconds (kernel) + toSeconds (user);
#else
    timespec now {};
    clock_gettime (CLOCK_THREAD_CPUTIME_ID, &now);
    return (double) now.tv_sec + (double) now.tv_nsec * 1.0e-9;
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
    timespec now {};
    clock_gettime (CLOCK_PROCESS_CPUTIME_ID, &now);
    return (double) now.tv_sec + (double) now.tv_nsec * 1.0e-9;
#endif
}
} // namespace phazite::test
