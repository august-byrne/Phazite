// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TimelineView.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>

namespace phazite::ui
{
double TimelineView::beatToX (BeatPosition beat) const
{
    return (beat - start).inBeats() * pixelsPerBeat;
}

BeatPosition TimelineView::xToBeat (double x) const
{
    return start + BeatDuration::fromBeats (x / pixelsPerBeat);
}

int TimelineView::beatToColumn (BeatPosition beat) const
{
    return (int) beatToEdge (beat, 1.0f);
}

double TimelineView::beatToEdge (BeatPosition beat, float scale) const
{
    // The view's start in whole pixels, and the part of one it is past them: none for a start on a whole pixel but for the
    // rounding of its beats, which would otherwise tip a line half a pixel in one way or the other.
    const auto startPixels = start.inBeats() * pixelsPerBeat;
    const auto whole = std::round (startPixels);
    const auto part = std::abs (startPixels - whole) < 1.0e-6 ? 0.0 : startPixels - whole;

    // The beat's own place on the screen's pixels, halves always up: rounded to the even, a line half a pixel in would go the
    // other way at every pixel the view moved.
    const auto perPixel = (double) std::max (1.0f, scale);
    const auto edge = std::floor ((beat.inBeats() * pixelsPerBeat - part) * perPixel + 0.5) / perPixel - whole;
    return std::clamp (edge, -1.0e9, 1.0e9);
}

void TimelineView::zoomAround (double x, double factor)
{
    const auto anchor = xToBeat (x);
    pixelsPerBeat = juce::jlimit (minPixelsPerBeat, maxPixelsPerBeat, pixelsPerBeat * factor);
    start = BeatPosition::fromBeats (std::max (0.0, anchor.inBeats() - x / pixelsPerBeat));
}

void TimelineView::scrollBy (double pixels)
{
    start = BeatPosition::fromBeats (std::max (0.0, start.inBeats() + pixels / pixelsPerBeat));
}
} // namespace phazite::ui
