// SPDX-License-Identifier: AGPL-3.0-or-later
// Automation as plain values (docs/design.md, section 10): a lane as the playlist shows it, the points on it in beats, the
// shapes that drawing places, and the write modes. AutomationModel.h reads and changes them in the open project.
#pragma once

#include "model/base/Beats.h"
#include "model/devices/ParameterRef.h"

#include <vector>

namespace phazite
{
// A lane: a parameter whose automation a track shows under it in the playlist. Every parameter with automation has one,
// so that automation never hides: a lane only collapses to a thin row.
struct AutomationLane
{
    static constexpr int minHeight = 32, defaultHeight = 48, maxHeight = 320;

    ParameterRef parameter {};
    int height = defaultHeight; // while it isn't collapsed, in pixels at an interface scale of 100%
    bool collapsed = false;
};

// A point where a parameter's automation changes. Between two points it goes from one's value to the next one's along a
// line, which the first one's curve bends; before the first point and after the last, it stays at theirs.
struct AutomationPoint
{
    BeatPosition beat {};
    float value = 0.0f; // in the parameter's own range

    // The line to the next point: 0 is straight; toward -maxBend or maxBend it bends one way or the other, still running
    // from this point to the next. Beyond, up to -1 or 1, the value holds at both ends with a shorter bend between them,
    // and at -1 or 1 it holds and then jumps, as discrete parameters' points do.
    float curve = 0.0f;

    static constexpr float maxBend = 0.5f; // the most bending a lane's line gives (Tracktion's curve)
};

//==============================================================================
// Shapes that drawing in a lane places, repeating across a range.
enum class AutomationShape
{
    line,     // from one value to the other, once
    sine,     // a smooth swing from one value to the other and back, each cycle
    triangle, // a straight swing there and back
    square,   // half a cycle at one value, half at the other
    sawUp,    // a ramp from the first value to the second, each cycle
    sawDown   // a ramp from the second value to the first
};

// The points of a shape across a range, swinging between two values (starting at from): a whole number of cycles, each
// as near the cycle length given as fits.
std::vector<AutomationPoint> makeAutomationShape (AutomationShape, BeatRange, BeatDuration cycle, float from, float to);

//==============================================================================
// Write modes: how moving a track's controls while the song plays writes their automation, as Tracktion writes it.
enum class AutomationMode
{
    read,  // the lanes play; a control moved by hand goes back to its lane at the lane's next change
    touch, // a control writes while it is held, and goes back to its lane when it is let go
    latch, // a control writes from when it is first moved until playback stops
    write  // from when playback starts, every automated parameter writes its control's value, until the control is let go;
           // then the track is in latch
};
} // namespace phazite
