// SPDX-License-Identifier: AGPL-3.0-or-later
// The frames' log (src/ui/timeline/FrameLog.h): what the display's frames did while the song played, written to Phazite's
// log once it stops.
#include "TestHelpers.h"
#include "ui/timeline/FrameLog.h"

#include <cmath>

namespace
{
constexpr double frameMilliseconds = 1000.0 / 60.0;

// Two seconds at 60 Hz, the playhead moving 0.8 pixels a frame (24 pixels a beat at 120 BPM), each frame taken a
// millisecond after its vertical blank and painted two after it; drawn where `drawn` puts it.
template <typename Drawn>
phazite::ui::FrameLog playFrames (Drawn&& drawn)
{
    phazite::ui::FrameLog log;

    for (int frame = 0; frame < 120; ++frame)
    {
        const auto vblank = 1000.0 + frame * frameMilliseconds;
        log.frame (vblank, vblank + 1.0, frame * 0.0333, drawn (frame), true);
        log.painted (vblank + 2.0);
    }

    return log;
}
} // namespace

TEST_CASE ("The frames' log tells a playhead drawn between pixels from one drawn on whole pixels")
{
    // Between pixels: it moves at every frame, by the same step.
    const auto even = playFrames ([] (int frame) { return 100.0 + frame * 0.8; }).getSummary();
    CHECK (even.frames == 120);
    CHECK (even.frameMilliseconds == doctest::Approx (frameMilliseconds));
    CHECK (even.seconds == doctest::Approx (119 * frameMilliseconds / 1000.0));
    CHECK (even.meanLate == doctest::Approx (1.0));
    CHECK (even.missed == 0);
    CHECK (even.paintedLate == 0);
    CHECK (even.longestPaintWait == doctest::Approx (2.0));
    CHECK (even.stoodStill == 0);
    CHECK (even.largestStepChange < 1.0e-6);

    // On whole pixels: it stands still one frame in five, and its step changes by a pixel there.
    const auto whole = playFrames ([] (int frame) { return std::round (100.0 + frame * 0.8); }).getSummary();
    CHECK (whole.stoodStill >= 20);
    CHECK (whole.largestStepChange == doctest::Approx (1.0));
}

TEST_CASE ("The frames' log counts vertical blanks missed and moves painted a frame late, and passes over jumps")
{
    phazite::ui::FrameLog log;
    auto vblank = 1000.0;
    auto x = 100.0, beat = 0.0;

    for (int frame = 0; frame < 60; ++frame)
    {
        // The message thread misses the 21st vertical blank: the 22nd comes two frames after the 20th.
        vblank += frame == 21 ? 2.0 * frameMilliseconds : frameMilliseconds;
        x += frame == 21 ? 1.6 : 0.8;
        beat += frame == 21 ? 0.0666 : 0.0333;

        // A loop's end at the 40th: the playhead goes back.
        if (frame == 40)
        {
            x -= 200.0;
            beat -= 8.0;
        }

        log.frame (vblank, vblank + 0.5, beat, x, true);

        // The 30th frame's move is painted only with the next frame's.
        if (frame != 30)
            log.painted (vblank + 1.5);
    }

    const auto summary = log.getSummary();
    CHECK (summary.missed == 1);
    CHECK (summary.paintedLate == 1);
    CHECK (summary.largestStepChange < 1.0e-6); // the jump at the loop's end, and the missed blank, aren't uneven steps

    // Its sentence names the editor and what it found, and nothing is kept after it.
    const auto text = log.takeSummary ("Playlist", "turning pages");
    CHECK (text.startsWith ("Frames, Playlist (turning pages): 60 in"));
    CHECK (text.contains ("1 missed"));
    CHECK (log.getFrames() == 0);
}
