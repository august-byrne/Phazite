// SPDX-License-Identifier: AGPL-3.0-or-later
// The conditions playback is tested at: a sample rate, a block size and a tempo each. What holds at 48 kHz and 120 BPM,
// where every beat is a whole number of samples and a block's ends fall where they always fall, need not hold where a beat
// falls between two samples (a loop's end rounds up or down, a click first sounds on the sample after its beat), where a
// block starts on a beat's first sample, or at another rate. A test of what playback does wherever it plays runs at each of
// them: atEveryCondition() names the condition in a failure, and EnginePlayback (EnginePlayback.h) plays at one.
// PlaybackConditionTests.cpp checks the engine's own promises at each.
#pragma once

#include "TestHelpers.h"

#include <array>
#include <cstdint>
#include <string>

namespace phazite::test
{
struct PlaybackCondition
{
    const char* name;
    int sampleRate; // in hertz
    int blockSize;  // the device's, in samples
    int tempo;      // in beats per minute, a beat a quarter note

    constexpr double getSampleRate() const noexcept { return (double) sampleRate; }
    constexpr double getTempo() const noexcept { return (double) tempo; }
    constexpr double getBeatSamples() const noexcept { return 60.0 * sampleRate / tempo; }

    // Whether every beat starts on a sample.
    constexpr bool hasWholeBeats() const noexcept { return (60 * (std::int64_t) sampleRate) % tempo == 0; }

    // The first sample at or after a beat, where its click starts and its notes play; and the sample nearest it (half a
    // sample rounding up), where the engine puts the playhead and a loop's ends. Worked out in whole numbers, so that
    // neither depends on how a division rounds.
    constexpr std::int64_t getFirstSampleOf (int beat) const noexcept
    {
        return (beat * 60 * (std::int64_t) sampleRate + tempo - 1) / tempo;
    }

    constexpr std::int64_t getNearestSampleTo (int beat) const noexcept
    {
        return (2 * beat * 60 * (std::int64_t) sampleRate + tempo) / (2 * tempo);
    }

    // The samples in a tenth of a second: a fingerprint's block at its rate (RenderHarness.h).
    constexpr int getFingerprintBlock() const noexcept { return sampleRate / 10; }
};

// Keep the list short: every test that uses it runs once for each. Each is there for what it alone shows.
inline constexpr std::array<PlaybackCondition, 5> playbackConditions { {
    // What the tests ran at alone before: a beat is 24000 samples.
    { "48 kHz in blocks of 512 at 120 BPM", 48000, 512, 120 },

    // The commonest rate, in blocks of 10 ms that aren't a power of two: a beat is 18900 samples, so every beat starts a block.
    { "44.1 kHz in blocks of 441 at 140 BPM", 44100, 441, 140 },

    // A beat is 20571.43 samples: beats 2, 4 and 6 round up to their nearest sample and 1, 3 and 5 down, so a loop's last
    // sample ends past its last beat or before it; in short blocks, so that many end near a beat.
    { "48 kHz in blocks of 64 at 140 BPM", 48000, 64, 140 },

    // A beat is 20671.875 samples: beat 1 first sounds on sample 20672, which is a block's first (323 blocks of 64).
    { "44.1 kHz in blocks of 64 at 128 BPM", 44100, 64, 128 },

    // A high rate in long blocks: a beat is 33103.45 samples.
    { "96 kHz in blocks of 1024 at 174 BPM", 96000, 1024, 174 },
} };

static_assert (playbackConditions[0].hasWholeBeats() && playbackConditions[1].hasWholeBeats());
static_assert (! playbackConditions[2].hasWholeBeats() && ! playbackConditions[3].hasWholeBeats());
static_assert (playbackConditions[2].getNearestSampleTo (2) == playbackConditions[2].getFirstSampleOf (2)); // rounds up
static_assert (playbackConditions[2].getNearestSampleTo (1) < playbackConditions[2].getFirstSampleOf (1));  // rounds down
static_assert (playbackConditions[3].getFirstSampleOf (1) % playbackConditions[3].blockSize == 0);

// Runs a check at each condition in turn; a check that fails says at which.
template <typename Check>
void atEveryCondition (Check&& check)
{
    for (const auto& condition : playbackConditions)
    {
        const std::string at = condition.name; // a name by its pointer would be captured as the pointer
        CAPTURE (at);
        check (condition);
    }
}
} // namespace phazite::test
