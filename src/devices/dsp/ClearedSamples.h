// SPDX-License-Identifier: AGPL-3.0-or-later
// What the devices' resets write as silence, counted for the tests. Every Play and every jump while the song plays resets
// every device of every track at once, on the audio thread and its workers (engine::Engine::flushStrip()), so what a reset
// writes must not grow with what a device holds: a delay line (DelayLine.h), a comb (CombFilter.h) and the EQ's linear
// phase (eq/LinearPhase.h, PartitionedConvolution.h) forget what they held by a count of what came in since, and write
// nothing of it. What the linear phase's convolution still clears at a reset goes through clear(), which counts it, and
// so should anything large a reset comes to clear, so that a test holds a block's resets to a small bound
// (tests/EnginePlaybackTests.cpp), as the engine's rings are held to none (engine/nodes/Strip.h). It counts only what goes
// through it: that a delay line's or a comb's reset writes nothing is kept by their own tests (tests/DspTests.cpp), which
// compare them with lines cleared whole, and by whoever changes them.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <span>

namespace phazite::devices::dsp
{
class ClearedSamples
{
public:
    // Silence over so many values (samples, or the complex bins of a spectrum), counted. Real-time safe.
    template <typename Value>
    static void clear (std::span<Value> values) noexcept
    {
        std::fill (values.begin(), values.end(), Value {});
        count.fetch_add ((std::int64_t) values.size(), std::memory_order_relaxed);
    }

    // For tests: how many values every reset has cleared since the program started.
    static std::int64_t get() noexcept { return count.load (std::memory_order_relaxed); }

private:
    static inline std::atomic<std::int64_t> count { 0 };
    static_assert (std::atomic<std::int64_t>::is_always_lock_free);
};
} // namespace phazite::devices::dsp
