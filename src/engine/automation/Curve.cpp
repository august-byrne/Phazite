// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Curve.h"
#include "devices/dsp/Math.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace phazite::engine
{
double getCurveProgress (double along, double curve) noexcept
{
    along = std::clamp (along, 0.0, 1.0);
    curve = std::clamp (curve, -1.0, 1.0);
    const auto amount = std::abs (curve);

    // Up to half way, the whole line eases, as far as the bend goes.
    if (amount <= 0.5)
        return devices::dsp::getEasedProgress (along, curve * 2.0);

    // Beyond, it holds for a part of the line, as much more as the bend is past half way, and eases all the way in the rest:
    // slow at first, it holds its first value at the start; quick at first, its last at the end. All the way (-1 or 1), it
    // jumps: at the line's end, or its start.
    const auto held = (amount - 0.5) * 2.0;
    const auto easing = 1.0 - held;

    if (curve > 0.0)
        return along <= held ? 0.0 : devices::dsp::getEasedProgress ((along - held) / easing, 1.0);

    return along >= easing ? 1.0 : devices::dsp::getEasedProgress (along / easing, -1.0);
}

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
    const auto length = to.beat - from.beat;

    if (length <= 0.0)
        return to.value;

    const auto progress = getCurveProgress ((beat - from.beat) / length, (double) from.curve);
    return from.value + (float) progress * (to.value - from.value);
}
} // namespace phazite::engine
