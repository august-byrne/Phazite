// SPDX-License-Identifier: AGPL-3.0-or-later
// A channel's level as Phazite's engine measures it, for the meters that show it (model/mixer/LevelSource.h): the
// loudest sample of each block, on each side, kept for the last blocks in a ring the audio thread writes and any number of
// readers read, each from where it read last. Lock-free.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace phazite::engine
{
class LevelMeter
{
public:
    static constexpr int maximumChannels = 2;

    // Audio thread: a block played, on one or two channels.
    void measure (const float* const* channels, int numChannels, int numSamples) noexcept;

    // Any reader: the loudest level of the blocks measured since the count it read last (where it gets the count to read
    // from next), as gain; and how many channels the last block had (0 before the first).
    struct Reader
    {
        std::array<std::uint64_t, maximumChannels> next {}; // for each channel
    };

    float getPeakSince (Reader&, int channel) const noexcept;
    int getNumChannels() const noexcept { return channelsUsed.load (std::memory_order_relaxed); }

private:
    static constexpr int ringSize = 64; // blocks: more than a meter waits between reads
    std::array<std::array<std::atomic<float>, ringSize>, maximumChannels> peaks {};
    std::atomic<std::uint64_t> blocks { 0 };
    std::atomic<int> channelsUsed { 0 };
};
} // namespace phazite::engine
