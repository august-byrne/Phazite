// SPDX-License-Identifier: AGPL-3.0-or-later
// What an editor shows of the time line: which part of the song is on screen, and how wide a beat is.
#pragma once

#include "model/arrangement/Grid.h"

namespace phazite::ui
{
// The visible part of the timeline: the beat at the left edge, and the zoom in pixels per beat.
struct TimelineView
{
    static constexpr double minPixelsPerBeat = 0.5;    // about 500 bars across a wide window
    static constexpr double maxPixelsPerBeat = 1024.0; // a 1/64 beat is 16 pixels wide
    static constexpr double defaultPixelsPerBeat = 24.0;

    te::BeatPosition start;
    double pixelsPerBeat = defaultPixelsPerBeat;

    double beatToX (te::BeatPosition) const;
    te::BeatPosition xToBeat (double x) const;

    // Zooms in (a factor above 1) or out, keeping the beat at x where it is. The zoom stays within its
    // limits, and the view doesn't start before the song does.
    void zoomAround (double x, double factor);

    // Moves the view sideways by some pixels (positive: later in the song), but not before beat 0.
    void scrollBy (double pixels);
};
} // namespace phazite::ui
