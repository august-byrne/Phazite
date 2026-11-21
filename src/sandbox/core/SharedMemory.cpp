// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SharedMemory.h"

#include <algorithm>
#include <chrono>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#endif
#endif

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace phazite::sandbox
{
namespace
{
    // A moment's pause in a spin, which lets the core's other thread on.
    void pause() noexcept
    {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
        _mm_pause();
#elif defined(__aarch64__)
        __asm__ __volatile__ ("yield");
#endif
    }

#if defined(_WIN32)
    // The names are Phazite's in this session's namespace.
    std::wstring toWindowsName (const std::string& name)
    {
        const std::string full = "Local\\phazite-" + name;
        return { full.begin(), full.end() };
    }
#else
    std::string toPosixName (const std::string& name)
    {
        return "/phz-" + name;
    }
#endif
} // namespace

std::int64_t getNanoseconds() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::steady_clock::now().time_since_epoch()).count();
}

//==============================================================================
std::unique_ptr<SharedMemory> SharedMemory::create (const std::string& name, std::size_t size)
{
    std::unique_ptr<SharedMemory> memory (new SharedMemory());
    memory->size = size;

#if defined(_WIN32)
    const auto windowsName = toWindowsName (name);
    auto* mapping = CreateFileMappingW (INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, (DWORD) ((std::uint64_t) size >> 32),
                                        (DWORD) (size & 0xffffffffu), windowsName.c_str());

    if (mapping == nullptr)
        return nullptr;

    // One by that name already: not ours to share.
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle (mapping);
        return nullptr;
    }

    memory->handle = mapping;
    memory->data = MapViewOfFile (mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
#else
    const auto posixName = toPosixName (name);
    const auto file = shm_open (posixName.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);

    if (file < 0)
        return nullptr;

    memory->unlinkName = posixName;

    if (ftruncate (file, (off_t) size) == 0)
    {
        auto* mapped = mmap (nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
        memory->data = mapped != MAP_FAILED ? mapped : nullptr;
    }

    close (file);
#endif

    return memory->data != nullptr ? std::move (memory) : nullptr;
}

std::unique_ptr<SharedMemory> SharedMemory::open (const std::string& name, std::size_t size)
{
    std::unique_ptr<SharedMemory> memory (new SharedMemory());
    memory->size = size;

#if defined(_WIN32)
    const auto windowsName = toWindowsName (name);
    auto* mapping = OpenFileMappingW (FILE_MAP_ALL_ACCESS, FALSE, windowsName.c_str());

    if (mapping == nullptr)
        return nullptr;

    memory->handle = mapping;
    memory->data = MapViewOfFile (mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
#else
    const auto file = shm_open (toPosixName (name).c_str(), O_RDWR, 0);

    if (file < 0)
        return nullptr;

    auto* mapped = mmap (nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
    memory->data = mapped != MAP_FAILED ? mapped : nullptr;
    close (file);
#endif

    return memory->data != nullptr ? std::move (memory) : nullptr;
}

SharedMemory::~SharedMemory()
{
#if defined(_WIN32)
    if (data != nullptr)
        UnmapViewOfFile (data);

    if (handle != nullptr)
        CloseHandle (handle);
#else
    if (data != nullptr)
        munmap (data, size);

    forgetName();
#endif
}

void SharedMemory::forgetName() noexcept
{
#if ! defined(_WIN32)
    if (! unlinkName.empty())
        shm_unlink (unlinkName.c_str());

    unlinkName.clear();
#endif
}

//==============================================================================
Bell::Bell (BellState& s)
    : state (s)
{
}

std::unique_ptr<Bell> Bell::create (const std::string& name, BellState& state)
{
    std::unique_ptr<Bell> bell (new Bell (state));

#if defined(_WIN32)
    const auto windowsName = toWindowsName (name);
    bell->event = CreateEventW (nullptr, FALSE, FALSE, windowsName.c_str()); // resets itself as it wakes its waiter

    if (bell->event == nullptr || GetLastError() == ERROR_ALREADY_EXISTS)
        return nullptr;

    bell->timer = CreateWaitableTimerExW (nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
#else
    (void) name;
#endif

    return bell;
}

std::unique_ptr<Bell> Bell::open (const std::string& name, BellState& state)
{
    std::unique_ptr<Bell> bell (new Bell (state));

#if defined(_WIN32)
    const auto windowsName = toWindowsName (name);
    bell->event = OpenEventW (EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, windowsName.c_str());

    if (bell->event == nullptr)
        return nullptr;

    bell->timer = CreateWaitableTimerExW (nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
#else
    (void) name;
#endif

    return bell;
}

Bell::~Bell()
{
#if defined(_WIN32)
    if (timer != nullptr)
        CloseHandle (timer);

    if (event != nullptr)
        CloseHandle (event);
#endif
}

void Bell::ring (std::uint32_t newValue) noexcept
{
    // Seen by a waiter that hasn't gone to sleep yet; one that has (it says so before it checks the counter again) is woken.
    state.value.store (newValue, std::memory_order_seq_cst);

    if (state.sleeping.load (std::memory_order_seq_cst) == 0)
        return;

#if defined(_WIN32)
    SetEvent (event);
#elif defined(__linux__)
    syscall (SYS_futex, reinterpret_cast<std::uint32_t*> (&state.value), FUTEX_WAKE, 1, nullptr, nullptr, 0);
#endif
}

bool Bell::waitWhile (std::uint32_t value, std::int64_t spinNanoseconds, std::int64_t timeoutNanoseconds) noexcept
{
    if (get() != value)
        return true;

    const auto start = getNanoseconds();

    if (spinNanoseconds > 0)
    {
        const auto spinUntil = start + (timeoutNanoseconds >= 0 ? std::min (spinNanoseconds, timeoutNanoseconds) : spinNanoseconds);

        do
        {
            pause();

            if (get() != value)
                return true;
        } while (getNanoseconds() < spinUntil);
    }

    const auto left = timeoutNanoseconds < 0 ? -1 : std::max<std::int64_t> (0, timeoutNanoseconds - (getNanoseconds() - start));
    return sleepWhile (value, left);
}

void Bell::wake() noexcept
{
    woken.store (true, std::memory_order_seq_cst);

#if defined(_WIN32)
    // The event stays set until its waiter takes it, if it doesn't sleep yet: it can't sleep through it.
    if (event != nullptr)
        SetEvent (event);
#elif defined(__linux__)
    // The futex word is the counter, which this doesn't move: a waiter that looked at the flag before it was set and has
    // yet to sleep would sleep through a single wake, so it is woken until it has taken the wake (or left its wait), and
    // no longer: one that goes straight into another wait stays waiting.
    while (waiting.load (std::memory_order_seq_cst) && woken.load (std::memory_order_seq_cst))
    {
        syscall (SYS_futex, reinterpret_cast<std::uint32_t*> (&state.value), FUTEX_WAKE, 1, nullptr, nullptr, 0);
        std::this_thread::yield();
    }
#endif
}

bool Bell::sleepWhile (std::uint32_t value, std::int64_t timeoutNanoseconds) noexcept
{
    const auto deadline = timeoutNanoseconds < 0 ? -1 : getNanoseconds() + timeoutNanoseconds;
    auto changed = false;
    waiting.store (true, std::memory_order_seq_cst);
    state.sleeping.store (1, std::memory_order_seq_cst);

    for (;;)
    {
        // Read as it says that it sleeps, and as ring() writes and looks: in one order for both threads, so that either
        // ring() sees it sleeping, or it sees what ring() wrote (an acquire load here could miss both, by the language's
        // rules, and sleep through a ring to its time limit).
        if (state.value.load (std::memory_order_seq_cst) != value)
        {
            changed = true;
            break;
        }

        // Woken by its own process (wake()): as though its time were up.
        if (woken.exchange (false, std::memory_order_seq_cst))
            break;

        const auto left = deadline < 0 ? -1 : deadline - getNanoseconds();

        if (deadline >= 0 && left <= 0)
            break;

#if defined(_WIN32)
        if (left < 0)
        {
            WaitForSingleObject (event, INFINITE);
        }
        else if (timer != nullptr)
        {
            LARGE_INTEGER due;
            due.QuadPart = -std::max<std::int64_t> (1, left / 100); // relative, in 100 ns
            SetWaitableTimer (timer, &due, 0, nullptr, nullptr, FALSE);
            HANDLE handles[] = { event, timer };

            // Woken by the event first: the timer stopped, which would otherwise still fire, an interrupt for nothing at
            // every block. One that fired before this was stopped is left signalled, and ends the next wait's sleep early
            // at most: that wait looks at the counter and its own time again, and sleeps on.
            if (WaitForMultipleObjects (2, handles, FALSE, INFINITE) == WAIT_OBJECT_0)
                CancelWaitableTimer (timer);
        }
        else
        {
            WaitForSingleObject (event, (DWORD) std::max<std::int64_t> (1, (left + 999999) / 1000000));
        }
#elif defined(__linux__)
        timespec relative {};

        if (left >= 0)
        {
            relative.tv_sec = (time_t) (left / 1000000000);
            relative.tv_nsec = (long) (left % 1000000000);
        }

        syscall (SYS_futex, reinterpret_cast<std::uint32_t*> (&state.value), FUTEX_WAIT, value, left >= 0 ? &relative : nullptr, nullptr,
                 0);
#else
        const timespec moment { 0, 50000 };
        nanosleep (&moment, nullptr);
#endif
    }

    state.sleeping.store (0, std::memory_order_seq_cst);
    waiting.store (false, std::memory_order_seq_cst);
    return changed;
}
} // namespace phazite::sandbox
