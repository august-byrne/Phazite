// SPDX-License-Identifier: AGPL-3.0-or-later
// A value that a device's editor shows on a meter, such as how far a compressor turns the sound down: the device raises
// it on the audio thread to the most each block reached, and the editor takes the most since it last looked, which
// starts it over. Lock-free, for one reader.
#pragma once

#include <atomic>

namespace phazite::devices
{
class MeterValue
{
public:
    explicit MeterValue (float lowestValue) noexcept
        : lowest (lowestValue),
          most (lowestValue)
    {
    }

    // Audio thread: what a block reached.
    void raise (float reached) noexcept
    {
        auto shown = most.load (std::memory_order_relaxed);

        // Only take() changes it meanwhile, to the lowest value, so this tries again only after a take().
        while (reached > shown && ! most.compare_exchange_strong (shown, reached, std::memory_order_relaxed))
        {
        }
    }

    // The most since the last call, or the lowest value when nothing came. One reader.
    float take() noexcept { return most.exchange (lowest, std::memory_order_relaxed); }

private:
    static_assert (std::atomic<float>::is_always_lock_free);

    const float lowest;
    std::atomic<float> most;
};
} // namespace phazite::devices
