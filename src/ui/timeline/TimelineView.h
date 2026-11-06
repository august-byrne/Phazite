// SPDX-License-Identifier: AGPL-3.0-or-later
// What an editor shows of the time line: which part of the song is on screen, and how wide a beat is.
#pragma once

#include "model/base/Beats.h"

namespace phazite::ui
{
// The visible part of the timeline: the beat at the left edge, and the zoom in pixels per beat.
struct TimelineView
{
    static constexpr double minPixelsPerBeat = 0.5;    // about 500 bars across a wide window
    static constexpr double maxPixelsPerBeat = 1024.0; // a 1/64 beat is 16 pixels wide
    static constexpr double defaultPixelsPerBeat = 24.0;

    // The latest beat a view starts at, however it is scrolled: far past any song (about 140 hours at 120 BPM), and where a
    // beat's place is still exact to well under a sample, so that what draws the time line from there ends (the owner,
    // 2026-11-06: a view carried to beats beyond that froze Phazite).
    static constexpr double latestStart = 1.0e6;

    BeatPosition start;
    double pixelsPerBeat = defaultPixelsPerBeat;

    double beatToX (BeatPosition) const;
    BeatPosition xToBeat (double x) const;

    // Zooms in (a factor above 1) or out, keeping the beat at x where it is. The zoom stays within its
    // limits, and the view doesn't start before the song does.
    void zoomAround (double x, double factor);

    // Moves the view sideways by some pixels (positive: later in the song), but not before beat 0.
    void scrollBy (double pixels);
};
} // namespace phazite::ui
