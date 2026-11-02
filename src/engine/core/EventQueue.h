// SPDX-License-Identifier: AGPL-3.0-or-later
// A queue of a fixed size that any number of threads push into and one takes from (the audio thread), without locks or
// allocation: Dmitry Vyukov's bounded queue, each cell with a count that says whose turn it is. For what comes to the engine
// from several threads at once, such as live notes (MIDI inputs' threads, the message thread).
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace phazite::engine
{
template <typename T, std::size_t capacity>
class EventQueue
{
    static_assert (capacity >= 2 && (capacity & (capacity - 1)) == 0, "a power of two");

public:
    EventQueue() noexcept
    {
        for (std::size_t i = 0; i < capacity; ++i)
            cells[i].turn.store (i, std::memory_order_relaxed);
    }

    // Any thread. False if the queue is full, when the value is dropped.
    bool push (const T& value) noexcept
    {
        auto position = pushed.load (std::memory_order_relaxed);

        for (;;)
        {
            auto& cell = cells[position & mask];
            const auto turn = cell.turn.load (std::memory_order_acquire);
            const auto difference = (std::intptr_t) turn - (std::intptr_t) position;

            if (difference == 0)
            {
                if (pushed.compare_exchange_weak (position, position + 1, std::memory_order_relaxed))
                {
                    cell.value = value;
                    cell.turn.store (position + 1, std::memory_order_release);
                    return true;
                }
            }
            else if (difference < 0)
            {
                return false;
            }
            else
            {
                position = pushed.load (std::memory_order_relaxed);
            }
        }
    }

    // The one thread that takes: the oldest value, if there is one.
    bool pop (T& value) noexcept
    {
        const auto position = popped.load (std::memory_order_relaxed);
        auto& cell = cells[position & mask];

        if (cell.turn.load (std::memory_order_acquire) != position + 1)
            return false;

        value = cell.value;
        cell.turn.store (position + capacity, std::memory_order_release);
        popped.store (position + 1, std::memory_order_relaxed);
        return true;
    }

private:
    static constexpr std::size_t mask = capacity - 1;

    struct Cell
    {
        std::atomic<std::size_t> turn { 0 };
        T value {};
    };

    std::array<Cell, capacity> cells;
    std::atomic<std::size_t> pushed { 0 }, popped { 0 };
};
} // namespace phazite::engine
