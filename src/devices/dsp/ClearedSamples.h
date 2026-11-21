// SPDX-License-Identifier: AGPL-3.0-or-later
// What the devices' resets write as silence, counted for the tests. Every Play and every jump while the song plays resets
// every device of every track at once, on the audio thread and its workers (engine::Engine::flushStrip()), so what a reset
// writes must cost little. The EQ's linear phase (eq/LinearPhase.h, PartitionedConvolution.h) forgets what it held by a
// count of what came in since, and writes none of it; what its convolution still clears at a reset goes through clear(),
// which counts it, so that a test holds a block's resets to a small bound of what is counted (tests/EnginePlaybackTests.cpp),
// as the engine's rings are held to none (engine/nodes/Strip.h). It counts only what goes through it: a delay line and a
// comb are cleared whole by plain loops (a count of what came in there costs a compare at every read, which made the
// reverb 40 % dearer to save a tenth of one block at Play: docs/realtime-audit.md, finding 25).
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
