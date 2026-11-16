// SPDX-License-Identifier: AGPL-3.0-or-later
// The clock that Phazite reads where a test must steer time: the timeouts that something must not end before (a
// compressor's Listen, a pad auditioned too late, a meter's peak hold, a burst of typing, Auto Save's wait), the CPU
// meters' measure of a device's time, and the engine's times of its blocks and its playhead's stamps (engine/core/Engine.h).
// It runs as JUCE's high-resolution clock (juce::Time::getHighResolutionTicks(), and
// juce::Time::getMillisecondCounterHiRes() for its milliseconds), unless a test holds it (phazite::test::HeldClock,
// tests/TestHelpers.h): then it stands still, moving only as far as the test moves it, so that what happens at a time, and
// not before it, doesn't depend on how busy the machine is or how late a thread woke. Let go, it runs on from where it stood,
// as far behind or ahead of JUCE's as the test left it: within a process it never goes back. Any thread, lock-free. The
// app, which never holds it, pays one relaxed atomic load more than JUCE's clock; the offset is read only once a test has
// held it.
#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>

namespace phazite::devices
{
class Clock
{
public:
    // Now, in the high-resolution clock's ticks.
    static std::int64_t getTicks() noexcept
    {
        const auto held = heldTicks.load (std::memory_order_relaxed);

        if (held == running)
            return juce::Time::getHighResolutionTicks();

        return held >= 0 ? held : juce::Time::getHighResolutionTicks() + getOffset();
    }

    // Now, in milliseconds, as juce::Time::getMillisecondCounterHiRes() counts them.
    static double getMilliseconds() noexcept
    {
        const auto held = heldTicks.load (std::memory_order_relaxed);

        if (held == running)
            return juce::Time::getMillisecondCounterHiRes();

        return held >= 0 ? toMilliseconds (held) : juce::Time::getMillisecondCounterHiRes() + toMilliseconds (getOffset());
    }

    // The same in whole milliseconds, wrapping as juce::Time::getMillisecondCounter() does (but not on its count: compare
    // it only with this clock's).
    static juce::uint32 getMillisecondCounter() noexcept { return (juce::uint32) (std::int64_t) getMilliseconds(); }

    // For tests (phazite::test::HeldClock): stands still from now on; moves on so many milliseconds while it stands still
    // (any thread: true if it did, false if it runs, which nothing moves); and runs again from where it stood.
    static void hold() noexcept { heldTicks.store (getTicks(), std::memory_order_relaxed); }
    static bool isHeld() noexcept { return heldTicks.load (std::memory_order_relaxed) >= 0; }

    static bool advance (double milliseconds) noexcept
    {
        const auto ticks = juce::Time::secondsToHighResolutionTicks (milliseconds / 1000.0);

        for (auto held = heldTicks.load (std::memory_order_relaxed); held >= 0;)
            if (heldTicks.compare_exchange_weak (held, held + ticks, std::memory_order_relaxed))
                return true;

        return false;
    }

    static void release() noexcept
    {
        for (auto held = heldTicks.load (std::memory_order_relaxed); held >= 0;)
        {
            offsetTicks.store (held - juce::Time::getHighResolutionTicks(), std::memory_order_relaxed);

            // The offset published with the release, which a reader that sees it acquires (getOffset()); an advance that
            // came first is taken into it by the next round.
            if (heldTicks.compare_exchange_weak (held, runningOffset, std::memory_order_release, std::memory_order_relaxed))
                return;
        }
    }

private:
    static_assert (std::atomic<std::int64_t>::is_always_lock_free);
    static constexpr std::int64_t running = -1;                    // as JUCE's, never held
    static constexpr std::int64_t runningOffset = -2;              // as JUCE's plus the offset, since a test let it go
    static inline std::atomic<std::int64_t> heldTicks { running }; // where it stands, while held (0 or more)
    static inline std::atomic<std::int64_t> offsetTicks { 0 };

    static std::int64_t getOffset() noexcept
    {
        std::atomic_thread_fence (std::memory_order_acquire); // after the load that saw runningOffset
        return offsetTicks.load (std::memory_order_relaxed);
    }

    static double toMilliseconds (std::int64_t ticks) noexcept { return juce::Time::highResolutionTicksToSeconds (ticks) * 1000.0; }
};
} // namespace phazite::devices
