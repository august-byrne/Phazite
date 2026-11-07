// SPDX-License-Identifier: AGPL-3.0-or-later
// Where the playhead was as each of the audio device's last blocks started, and when, so that the window can read it at its
// frames' own times (Engine::getPlayheadBeatAt(); docs/performance-audit.md, section 16). The times are smoothed by a
// delay-locked loop over the times the blocks come at (F. Adriaensen, "Using a DLL to filter time", 2005, as JACK and
// PipeWire smooth their clocks): a block that comes a little early or late, or several that come together, as some devices
// and Bluetooth outputs hand them, keep the times of a steady stream. Real-time safe: no allocation, no lock.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>

namespace phazite::engine
{
// When blocks start, in milliseconds by juce::Time::getMillisecondCounterHiRes(), smoothed. The audio thread's.
class BlockClock
{
public:
    // The time the block that comes now starts at: given when it came, how many samples it has and the sample rate. It
    // starts again from the time given at a first block, after the rate changes, and at a block that comes further from its
    // time than blocks that come together would (a dropout, the device stopped and started again, blocks played faster than
    // real time, as a render's and the tests' are).
    double stamp (double milliseconds, int numSamples, double sampleRate) noexcept;

    // How quickly it follows a change in the blocks' timing (Hz): settled in a second or two, after which a block's own
    // lateness moves the times by a few percent of it; and how many blocks' lengths from its time a block may come before
    // it starts again.
    static constexpr double bandwidth = 0.5;
    static constexpr double farthest = 8.0;

private:
    bool running = false;
    double rate = 0.0;
    double next = 0.0;      // when the next block should start
    double perSample = 0.0; // milliseconds, as the blocks have come
};

// Where the playhead was as the last blocks started: written by one thread at a time (the audio thread, or the message
// thread while no block plays), read by any.
class PlayheadStamps
{
public:
    struct Stamp
    {
        double beat = 0.0;                // where the block starts
        double milliseconds = 0.0;        // when (BlockClock)
        double beatsPerMillisecond = 0.0; // how fast the playhead moves through it: 0 while stopped or counting in
        double length = 0.0;              // how long it lasts, in milliseconds
        std::uint32_t run = 0;            // moved on by each locate: a look-up doesn't reach back across a locate
    };

    void publish (const Stamp&) noexcept;
    std::optional<Stamp> getLatest() const noexcept; // nothing before a first block

    // Where the playhead was, or is, at a moment: in the latest block that had started by then, by the time since its start,
    // to its end at most (the latest block's by half a block more, for a next block that comes a little late); before the
    // first block it keeps since the last locate, that block's beat. Nothing before a first block.
    std::optional<double> getBeatAt (double milliseconds) const noexcept;

    // How many blocks it keeps: 2.7 s of 10 ms blocks, 0.7 s of 128-sample blocks at 48 kHz, more than any output's delay
    // to look back over.
    static constexpr std::uint64_t size = 256;

private:
    // Each slot between two counts of its sequence: odd while it is written, and 2n + 2 once it holds stamp number n, so
    // that a reader knows it read one stamp whole, and which one.
    struct Slot
    {
        std::atomic<std::uint64_t> sequence { 0 };
        std::atomic<double> beat { 0.0 }, milliseconds { 0.0 }, beatsPerMillisecond { 0.0 }, length { 0.0 };
        std::atomic<std::uint32_t> run { 0 };
    };

    static_assert (std::atomic<double>::is_always_lock_free && std::atomic<std::uint64_t>::is_always_lock_free);

    std::array<Slot, size> slots;
    std::atomic<std::uint64_t> written { 0 }; // how many stamps were published

    bool read (std::uint64_t number, Stamp&) const noexcept;
};
} // namespace phazite::engine
