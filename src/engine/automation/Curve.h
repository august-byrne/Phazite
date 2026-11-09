// SPDX-License-Identifier: AGPL-3.0-or-later
// A parameter's automation as Phazite's engine plays it, and the lanes draw it: its points in the song's beats, each with
// how the line to the next one bends (from -1 to 1): a straight line at 0; up to half way, the line eased (devices/dsp/
// Math.h: getEasedProgress()), quick at first below 0 and slow at first above; beyond, held at one end for a part of the
// line, as much more as the bend goes further, and eased all the way in the rest; and at -1 or 1, a jump and a hold.
// Before the first point and after the last, the value stays at theirs. Built on the message thread from the song's points
// and never changed after, so the audio thread reads it as it is.
#pragma once

#include <vector>

namespace phazite::engine
{
// The control rate: automation and modulation move what they move on the song's control points, one every so many
// samples from its start, so that a song sounds the same at any block size (docs/performance-audit.md, A7;
// docs/design.md, section 10).
inline constexpr int controlPointSpacing = 32;

struct CurvePoint
{
    double beat = 0.0;
    float value = 0.0f;
    float curve = 0.0f; // -1 to 1, 0 for a straight line to the next point
};

// How far a line from one point to the next has gone from the first's value to the second's (0 to 1), a part of the way
// along it (0 to 1), for the first point's curve (-1 to 1): what Curve plays, and the song's tempo map's lines too
// (core/TempoMap.h).
double getCurveProgress (double along, double curve) noexcept;

class Curve
{
public:
    // Points in time order; two at one beat are a jump, the first's value up to the beat and the second's from it.
    explicit Curve (std::vector<CurvePoint>);

    bool isEmpty() const noexcept { return points.empty(); }
    const std::vector<CurvePoint>& getPoints() const noexcept { return points; }

    // Its value at a beat. Real-time safe: a binary search, no allocation.
    float getValueAt (double beat) const noexcept;

private:
    std::vector<CurvePoint> points;
};
} // namespace phazite::engine
