// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's loudness as Phazite's engine measures what goes out, by EBU R128 and ITU-R BS.1770-4: through the
// K-weighting filters, the momentary loudness (the last 400 ms) and the short-term (the last 3 s), the integrated
// loudness since it was last reset, gated as R128 gates it (blocks quieter than -70 LUFS, then those more than 10 LU
// below the rest, left out; the blocks kept in a histogram of tenths of a LU, so that its memory stays the same however
// long it measures), and the true peak, four times oversampled, held. The audio thread measures; any thread reads.
#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>

namespace phazite::engine
{
class LoudnessMeter
{
public:
    // Before it measures, off the audio thread (it allocates).
    void prepare (double sampleRate, int maximumBlockSize);

    // Audio thread: measures a block of two channels (or one).
    void process (const float* const* channels, int numChannels, int numSamples) noexcept;

    // Any thread: starts the integrated loudness and the held peak again, at the audio thread's next block.
    void reset() noexcept { resetWanted.store (true, std::memory_order_release); }

    // Any thread: in LUFS and dBTP, each none until it has something to say (400 ms for the momentary, 3 s for the
    // short-term, a block that passes the gates for the integrated, a sound for the peak).
    struct Readings
    {
        std::optional<float> momentary, shortTerm, integrated, truePeak;
    };

    Readings getReadings() const noexcept;

private:
    struct Biquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;

        double process (double x) noexcept
        {
            const auto y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    static constexpr int blocksInMomentary = 4, blocksInShortTerm = 30; // of 100 ms
    static constexpr int histogramBins = 800;                           // tenths of a LU, from -70 LUFS to +10
    static constexpr double histogramFloor = -70.0;

    double sampleRate = 48000.0;
    int blockLength = 4800; // 100 ms
    std::array<std::array<Biquad, 2>, 2> filters;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    int maximumBlockSize = 0;

    // The audio thread's: the K-weighted power of the 100 ms block being measured, and of the last thirty; the gating
    // blocks' histogram (each bin's count and power); the held peak.
    double blockPower = 0.0;
    int blockDone = 0, blocksMeasured = 0, blocksSinceReset = 0; // only whole gating blocks since a reset count
    std::array<double, blocksInShortTerm> lastBlocks {};
    std::vector<double> binPower;
    std::vector<int> binCount;
    float heldPeak = 0.0f;

    std::atomic<bool> resetWanted { false };
    std::atomic<float> momentary { -1000.0f }, shortTerm { -1000.0f }, integrated { -1000.0f }, truePeak { 0.0f };

    void finishBlock() noexcept;
    void startAgain() noexcept;
};
} // namespace phazite::engine
