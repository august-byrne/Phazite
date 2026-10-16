// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TimelineView.h"

#include <juce_core/juce_core.h>

#include <algorithm>

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
