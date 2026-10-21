// SPDX-License-Identifier: AGPL-3.0-or-later
// A value a device's editor shows as it is now, such as where an LFO is in its cycle or how long a delay's time is as it
// glides: the device sets it on the audio thread, once a block, and the editor reads the latest. Lock-free; any number of
// readers.
#pragma once

#include <atomic>

namespace phazite::devices
{
class LiveValue
{
public:
    explicit LiveValue (float initialValue = 0.0f) noexcept
        : value (initialValue)
    {
    }

    void set (float newValue) noexcept { value.store (newValue, std::memory_order_relaxed); } // audio thread
    float get() const noexcept { return value.load (std::memory_order_relaxed); }

private:
    static_assert (std::atomic<float>::is_always_lock_free);

    std::atomic<float> value;
};
} // namespace phazite::devices
