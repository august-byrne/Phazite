// SPDX-License-Identifier: AGPL-3.0-or-later
// What two processes share: memory made by one under a name and opened by the other by it, mapped into both for as long
// as each keeps it; and a bell, a counter in that memory that one side moves on and the other waits on, woken by the
// operating system only when it sleeps (Windows: an event of the same name; elsewhere a futex on the counter itself).
// Real-time safe once made: ringing and waiting allocate nothing and take no lock.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace phazite::sandbox
{
class SharedMemory
{
public:
    // Made anew, zeroed, under a name of letters, digits and '-' of at most 24 characters; or opened as the other
    // process made it. nullptr if it couldn't be.
    static std::unique_ptr<SharedMemory> create (const std::string& name, std::size_t size);
    static std::unique_ptr<SharedMemory> open (const std::string& name, std::size_t size);
    ~SharedMemory();

    void* getData() const noexcept { return data; }
    std::size_t getSize() const noexcept { return size; }

    SharedMemory (const SharedMemory&) = delete;
    SharedMemory& operator= (const SharedMemory&) = delete;

private:
    SharedMemory() = default;

    void* data = nullptr;
    std::size_t size = 0;
    void* handle = nullptr; // Windows' mapping
    std::string unlinkName; // elsewhere, the maker's name, unlinked as it goes
};

// A counter in shared memory, and whether its waiter sleeps (so that a ring wakes it only then).
struct BellState
{
    std::atomic<std::uint32_t> value { 0 };
    std::atomic<std::uint32_t> sleeping { 0 };
};

static_assert (std::atomic<std::uint32_t>::is_always_lock_free);

class Bell
{
public:
    // The bell for a counter in shared memory: made by one side under a name of at most 24 characters (on Windows, the
    // event's), opened by the other by it.
    static std::unique_ptr<Bell> create (const std::string& name, BellState&);
    static std::unique_ptr<Bell> open (const std::string& name, BellState&);
    ~Bell();

    std::uint32_t get() const noexcept { return state.value.load (std::memory_order_acquire); }

    // Moves the counter to a new value, waking the other side if it sleeps on it.
    void ring (std::uint32_t newValue) noexcept;

    // Waits until the counter isn't the value given: spinning for spinNanoseconds, then asleep until it changes or the
    // timeout passes (negative: no timeout). False if the timeout passed first.
    bool waitWhile (std::uint32_t value, std::int64_t spinNanoseconds, std::int64_t timeoutNanoseconds) noexcept;

    Bell (const Bell&) = delete;
    Bell& operator= (const Bell&) = delete;

private:
    explicit Bell (BellState&);

    BellState& state;
    void* event = nullptr; // Windows: the event the waiter sleeps on, and a high-resolution timer for its timeout
    void* timer = nullptr;

    bool sleepWhile (std::uint32_t value, std::int64_t timeoutNanoseconds) noexcept;
};

// The time now in nanoseconds, from a clock that only moves on.
std::int64_t getNanoseconds() noexcept;
} // namespace phazite::sandbox
