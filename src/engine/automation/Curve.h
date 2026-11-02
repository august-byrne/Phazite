// SPDX-License-Identifier: AGPL-3.0-or-later
// A parameter's automation as Phazite's engine plays it (docs/tracktion-migration.md, T2): its points in the song's beats,
// each with how the line to the next one bends, worked out as Tracktion's curves are, so that a lane sounds as it is
// drawn: a straight line; a bend up to half way (a quadratic Bezier curve through a point moved off the line's middle);
// beyond, the value held at both ends with a shorter bend between them; and at the ends of the bend's range, a hold and
// a jump. Before the first point and after the last, the value stays at theirs. Built on the message thread from the
// song's points and never changed after, so the audio thread reads it as it is.
#pragma once

#include <vector>

namespace phazite::engine
{
// The control rate: automation and modulation move what they move on the song's control points, one every so many samples
// from its start, so that a song sounds the same at any block size (docs/tracktion-migration.md, T2: "one control rate").
inline constexpr int controlPointSpacing = 32;

struct CurvePoint
{
    double beat = 0.0;
    float value = 0.0f;
    float curve = 0.0f; // -1 to 1, 0 for a straight line to the next point
};

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
