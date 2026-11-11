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

std::vector<TimelineView::LineSample> TimelineView::getLineSamples (int firstColumn, int lastColumn, int step,
                                                                    const std::vector<BeatPosition>& pointBeats) const
{
    // The song's own pixels, counted from its start, a step apart, from one step before the first column to one after the
    // last: the view's start in whole pixels is where beatToEdge() counts its columns from.
    step = std::max (1, step);
    const auto whole = std::round (start.inBeats() * pixelsPerBeat);
    const auto first = std::max (0.0, std::floor ((whole + firstColumn) / step) * step - step);
    const auto last = std::ceil ((whole + lastColumn) / step) * step + step;
    std::vector<LineSample> samples;

    if (last < first)
        return samples;

    samples.reserve ((size_t) ((last - first) / step) + 2 * pointBeats.size() + 1);

    for (auto pixel = first; pixel <= last; pixel += step)
    {
        const auto beat = BeatPosition::fromBeats (pixel / pixelsPerBeat);
        samples.push_back ({ beat, beatToColumn (beat) });
    }

    // Each point's beat, where the line may jump: what it comes in at, a moment before, and leaves at, in one column. A
    // billionth of a beat is far under a sample, and over the rounding of any beat a view reaches (latestStart).
    constexpr double justBefore = 1.0e-9;
    const auto from = samples.front().beat, to = samples.back().beat;

    for (const auto beat : pointBeats)
    {
        if (beat < from || beat > to)
            continue;

        const auto column = beatToColumn (beat);
        samples.push_back ({ BeatPosition::fromBeats (beat.inBeats() - justBefore), column });
        samples.push_back ({ beat, column });
    }

    std::stable_sort (samples.begin(), samples.end(), [] (const LineSample& a, const LineSample& b) { return a.beat < b.beat; });
    return samples;
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
