// SPDX-License-Identifier: AGPL-3.0-or-later
// A parameter's automation as Phazite's engine plays it, and the lanes draw it: its points in the song's beats, each at a
// position along the parameter's control's travel (0 to 1, as its knob or fader moves), with how the line to the next one
// bends (from -1 to 1): a straight line at 0; up to half way, the line eased (devices/dsp/Math.h: getEasedProgress()),
// quick at first below 0 and slow at first above; beyond, held at one end for a part of the line, as much more as the bend
// goes further, and eased all the way in the rest; and at -1 or 1, a jump and a hold. So a straight line is an even turn of
// the knob, as other DAWs play automation: on a cutoff's logarithmic travel, a line sweeps evenly through the octaves.
// Before the first point and after the last, the position stays at theirs. Built on the message thread from the song's
// points and never changed after, so the audio thread reads it as it is.
#pragma once

#include "devices/core/DeviceInfo.h"

#include <vector>

namespace phazite::engine
{
// The control rate: automation and modulation move what they move on the song's control points, one every so many
// samples from its start, so that a song sounds the same at any block size (docs/archive/performance-audit.md, A7;
// docs/design.md, section 10).
inline constexpr int controlPointSpacing = 32;

struct CurvePoint
{
    double beat = 0.0;
    float position = 0.0f; // along the control's travel, 0 to 1
    float curve = 0.0f;    // -1 to 1, 0 for a straight line to the next point
};

// How far a line from one point to the next has gone from the first's position to the second's (0 to 1), a part of the
// way along it (0 to 1), for the first point's curve (-1 to 1): what Curve plays, and the song's tempo map's lines too
// (core/TempoMap.h).
double getCurveProgress (double along, double curve) noexcept;

class Curve
{
public:
    // Points in time order; two at one beat are a jump, the first's position up to the beat and the second's from it. The
    // travel is the parameter's as its control spans it (devices::getTravel()), linear, skewed or logarithmic: what turns a
    // position into the parameter's own units; 0 to 1 for a parameter that is a position itself (a fader's volume, a
    // plug-in's). Plain numbers: no copy of it allocates.
    explicit Curve (std::vector<CurvePoint>, devices::Travel travel = {});

    bool isEmpty() const noexcept { return points.empty(); }
    const std::vector<CurvePoint>& getPoints() const noexcept { return points; }

    // Its position at a beat, and the parameter's value there in its own units (hertz, a pan from -1 to 1), not yet on its
    // steps: its runner keeps it to them (devices::constrain()). Real-time safe: a binary search, no allocation.
    float getPositionAt (double beat) const noexcept;
    float getValueAt (double beat) const noexcept;

private:
    std::vector<CurvePoint> points;
    devices::Travel travel;
};
} // namespace phazite::engine
