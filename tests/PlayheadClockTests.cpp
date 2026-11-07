// SPDX-License-Identifier: AGPL-3.0-or-later
// The playhead's clock (src/engine/core/PlayheadClock.h): the audio device's blocks timed as a steady stream however they
// come, and the playhead looked up in the block that played at a moment, as the window's frames ask for it.
#include "TestHelpers.h"
#include "engine/core/PlayheadClock.h"

#include <algorithm>
#include <vector>

using phazite::engine::BlockClock;
using phazite::engine::PlayheadStamps;

namespace
{
// How far a block's time strays from a steady stream's over a stretch of blocks: the spread of each time less its block's
// place in the stream.
double getSpread (const std::vector<double>& times, double blockMilliseconds, size_t from)
{
    auto lowest = 1.0e18, highest = -1.0e18;

    for (auto block = from; block < times.size(); ++block)
    {
        const auto offset = times[block] - (double) block * blockMilliseconds;
        lowest = std::min (lowest, offset);
        highest = std::max (highest, offset);
    }

    return highest - lowest;
}
} // namespace

TEST_CASE ("Blocks that come together or a little late are timed as a steady stream")
{
    // A device that hands four 128-sample blocks at once at its period of four blocks (shared-mode WASAPI at 128 samples),
    // waking up to half a millisecond late: as they come, their times spread over more than 8 ms around a steady stream's,
    // which made the playhead jump a block and wait one. Once settled, their stamps keep within half a millisecond of one.
    constexpr double rate = 48000.0;
    constexpr int samples = 128;
    const auto length = samples * 1000.0 / rate;
    BlockClock clock;
    juce::Random random (7);
    std::vector<double> came, stamped;

    for (int block = 0; block < 6000; ++block) // 16 seconds
    {
        const auto burst = block / 4;
        const auto time = 1000.0 + burst * 4 * length + random.nextDouble() * 0.5 + (block % 4) * 0.02;
        came.push_back (time);
        stamped.push_back (clock.stamp (time, samples, rate));
    }

    const auto settled = (size_t) (3000.0 / length); // after three seconds
    CHECK (getSpread (came, length, settled) > 8.0);
    CHECK (getSpread (stamped, length, settled) < 0.5);

    // A block 120 ms late (a dropout, the device started again) starts the clock again from when it came.
    const auto late = came.back() + length + 120.0;
    CHECK (clock.stamp (late, samples, rate) == doctest::Approx (late));

    // Blocks played faster than real time, as a render's and the tests' are, are timed as they come every few blocks.
    BlockClock fast;
    auto restarts = 0;

    for (int block = 0; block < 200; ++block)
        if (const auto time = 1000.0 + block * 0.05; std::abs (fast.stamp (time, 256, rate) - time) < 1.0e-9)
            ++restarts;

    CHECK (restarts > 10);
}

TEST_CASE ("The playhead at a moment is found in the block that played then, back to the last locate")
{
    PlayheadStamps stamps;
    CHECK_FALSE (stamps.getBeatAt (1000.0).has_value()); // before a first block
    CHECK_FALSE (stamps.getLatest().has_value());

    // Ten blocks of 10 ms from beat 0 at 120 BPM (0.002 beats a millisecond), the first at 1000 ms.
    for (int block = 0; block < 10; ++block)
        stamps.publish ({ block * 0.02, 1000.0 + block * 10.0, 0.002, 10.0, 0 });

    REQUIRE (stamps.getLatest().has_value());
    CHECK (stamps.getLatest()->beat == doctest::Approx (0.18));
    CHECK (*stamps.getBeatAt (1025.0) == doctest::Approx (0.05));         // half way through the third block
    CHECK (*stamps.getBeatAt (1090.0 + 12.0) == doctest::Approx (0.204)); // after the latest, for a next block a little late
    CHECK (*stamps.getBeatAt (1090.0 + 40.0) == doctest::Approx (0.21));  // but no further than half a block past its end
    CHECK (*stamps.getBeatAt (500.0) == doctest::Approx (0.0));           // before the first: its beat

    // A loop's end: the next block starts at the loop's start, and a moment before it still finds the loop's end.
    stamps.publish ({ 1.0, 1100.0, 0.002, 10.0, 0 });
    CHECK (*stamps.getBeatAt (1095.0) == doctest::Approx (0.19));
    CHECK (*stamps.getBeatAt (1105.0) == doctest::Approx (1.01));

    // A locate begins another run: a moment before its first block finds that block, not where the playhead was.
    stamps.publish ({ 8.0, 1110.0, 0.002, 10.0, 1 });
    CHECK (*stamps.getBeatAt (1105.0) == doctest::Approx (8.0));
    CHECK (*stamps.getBeatAt (1115.0) == doctest::Approx (8.01));

    // Stopped, it stays where it is; and it keeps the latest blocks only, looking no further back than them.
    stamps.publish ({ 8.02, 1120.0, 0.0, 10.0, 1 });
    CHECK (*stamps.getBeatAt (1500.0) == doctest::Approx (8.02));

    for (int block = 0; block < 1000; ++block)
        stamps.publish ({ 10.0 + block * 0.02, 2000.0 + block * 10.0, 0.002, 10.0, 2 });

    const auto oldestKept = 1000 - (int) PlayheadStamps::size + 1;
    CHECK (*stamps.getBeatAt (2000.0) == doctest::Approx (10.0 + oldestKept * 0.02));
}
