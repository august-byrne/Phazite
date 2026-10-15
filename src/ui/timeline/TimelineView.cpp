// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TimelineView.h"

#include <algorithm>

namespace phazite::ui
{
double TimelineView::beatToX (te::BeatPosition beat) const
{
    return (beat - start).inBeats() * pixelsPerBeat;
}

te::BeatPosition TimelineView::xToBeat (double x) const
{
    return start + te::BeatDuration::fromBeats (x / pixelsPerBeat);
}

void TimelineView::zoomAround (double x, double factor)
{
    const auto anchor = xToBeat (x);
    pixelsPerBeat = juce::jlimit (minPixelsPerBeat, maxPixelsPerBeat, pixelsPerBeat * factor);
    start = te::BeatPosition::fromBeats (std::max (0.0, anchor.inBeats() - x / pixelsPerBeat));
}

void TimelineView::scrollBy (double pixels)
{
    start = te::BeatPosition::fromBeats (std::max (0.0, start.inBeats() + pixels / pixelsPerBeat));
}
} // namespace phazite::ui
