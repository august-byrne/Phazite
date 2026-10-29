// SPDX-License-Identifier: AGPL-3.0-or-later
// A value that any number of displays show as it is now, such as how far a compressor turns the sound down, which the
// mixer's slot and the Devices view's card show at once: the device raises it to what each block reached, and it falls back
// by a steady amount a second from there, so that a display looking a few times a second still sees a short peak, falling
// away, and one whose device sleeps on silence (and so pushes nothing) sees it fall to nothing. Lock-free.
#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>

namespace phazite::devices
{
class FallingValue
{
public:
    explicit FallingValue (float fallPerSecond) noexcept
        : fall (fallPerSecond)
    {
    }

    // Audio thread: what a block reached (0 or more), by the clock now.
    void push (float reached) noexcept
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto shown = std::max (reached, valueAt (now));
        value.store (shown, std::memory_order_relaxed);
        time.store (now, std::memory_order_relaxed);
    }

    // Any thread: as it is now, fallen since it was last pushed, down to 0.
    float get() const noexcept { return valueAt (juce::Time::getMillisecondCounterHiRes()); }

private:
    static_assert (std::atomic<float>::is_always_lock_free);
    static_assert (std::atomic<double>::is_always_lock_free);

    const float fall;
    std::atomic<float> value { 0.0f };
    std::atomic<double> time { 0.0 }; // when it was pushed, in the clock's milliseconds

    float valueAt (double now) const noexcept
    {
        const auto seconds = (float) std::max (0.0, now - time.load (std::memory_order_relaxed)) / 1000.0f;
        return std::max (0.0f, value.load (std::memory_order_relaxed) - fall * seconds);
    }
};
} // namespace phazite::devices
