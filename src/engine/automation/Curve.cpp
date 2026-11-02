// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Curve.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace phazite::engine
{
namespace
{
    // The point a bend's Bezier curve goes through instead of the line's middle, for a bend from -1 to 1 (twice the point's).
    std::pair<double, double> getBendPoint (double x1, double y1, double x2, double y2, double bend) noexcept
    {
        const auto run = x2 - x1;
        const auto x = x1 + run / 2 + run / 2 * bend;

        if (y2 > y1)
            return { x, y1 + (y2 - y1) / 2 - (y2 - y1) / 2 * bend };

        return { x, y2 + (y1 - y2) / 2 + (y1 - y2) / 2 * bend };
    }

    // A quadratic Bezier curve's value where it is at x, from (x1, y1) through the pull of (xb, yb) to (x2, y2).
    double getBezierValueAt (double x, double x1, double y1, double xb, double yb, double x2, double y2) noexcept
    {
        if (juce::exactlyEqual (x1, x2) || juce::exactlyEqual (y1, y2)) // a flat line, or none
            return y1;

        if (x <= x1)
            return y1;

        if (x >= x2)
            return y2;

        // x along the curve is a quadratic in t, from 0 to 1: its root in that range.
        const auto a = x1 - 2 * xb + x2;
        const auto b = -2 * x1 + 2 * xb;
        const auto c = x1 - x;
        double t;

        if (juce::exactlyEqual (a, 0.0))
        {
            t = -c / b;
        }
        else
        {
            const auto root = std::sqrt (b * b - 4 * a * c);
            t = (-b + root) / (2 * a);

            if (t < 0.0 || t > 1.0)
                t = (-b - root) / (2 * a);
        }

        return (1 - t) * (1 - t) * y1 + 2 * t * (1 - t) * yb + t * t * y2;
    }
} // namespace

Curve::Curve (std::vector<CurvePoint> p)
    : points (std::move (p))
{
}

float Curve::getValueAt (double beat) const noexcept
{
    // The first point after the beat: at a point, the line from it plays; at a jump, the line from its second point.
    const auto next = std::upper_bound (points.begin(), points.end(), beat, [] (double b, const CurvePoint& p) { return b < p.beat; });

    if (next == points.begin())
        return points.empty() ? 0.0f : points.front().value;

    if (next == points.end())
        return points.back().value;

    const auto& from = *(next - 1);
    const auto& to = *next;
    const auto x1 = from.beat, x2 = to.beat;
    const auto y1 = (double) from.value, y2 = (double) to.value;
    const auto bend = from.curve;

    if (juce::exactlyEqual (bend, 0.0f))
        return from.value + (float) ((beat - x1) / (x2 - x1)) * (to.value - from.value);

    // Up to half way: one Bezier curve from point to point.
    const auto [xb, yb] = getBendPoint (x1, y1, x2, y2, std::clamp ((double) bend * 2.0, -1.0, 1.0));

    if (bend >= -0.5f && bend <= 0.5f)
        return (float) getBezierValueAt (beat, x1, y1, xb, yb, x2, y2);

    // Beyond: held at both ends, the bend between them as much shorter as the bend is greater; at 1 or -1, a hold and a
    // jump. Its ends' values are floats, as Tracktion keeps them.
    const auto shorter = (std::abs ((double) bend) - 0.5) * 2.0;
    const auto run = shorter * (x2 - x1);
    const auto rise = shorter * std::abs (y2 - y1);
    double endX1, endX2, endY1, endY2;

    if (bend > 0.0f)
    {
        endX1 = x1 + run;
        endY1 = (float) y1;
        endX2 = x2;
        endY2 = (float) (y1 < y2 ? y2 - rise : y2 + rise);
    }
    else
    {
        endX1 = x1;
        endY1 = (float) (y1 < y2 ? y1 + rise : y1 - rise);
        endX2 = x2 - run;
        endY2 = (float) y2;
    }

    if (beat >= x1 && beat <= endX1)
        return from.value;

    if (beat >= endX2 && beat <= x2)
        return to.value;

    return (float) getBezierValueAt (beat, endX1, endY1, xb, yb, endX2, endY2);
}
} // namespace phazite::engine
