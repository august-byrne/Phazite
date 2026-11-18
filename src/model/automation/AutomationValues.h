// SPDX-License-Identifier: AGPL-3.0-or-later
// Automation as plain values (docs/design.md, section 10): a lane as the playlist shows it, the points on it in beats, the
// shapes that drawing places, and the write modes. AutomationModel.h reads and changes them in the open project.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "model/base/Beats.h"
#include "model/devices/ParameterRef.h"

#include <juce_core/juce_core.h>

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

// A point where a parameter's automation changes. Between two points it goes from one's position to the next one's along a
// line, which the first one's curve bends; before the first point and after the last, it stays at theirs.
struct AutomationPoint
{
    BeatPosition beat {};

    // Where the parameter's control is there, along its travel, from 0 (all the way down or left) to 1, as its knob or
    // fader moves: a straight line is an even turn of the knob, as in Ableton Live and most DAWs, so that a sweep of a
    // cutoff, whose knob is logarithmic (devices::Scale), goes evenly through the octaves. toControlPosition() and
    // fromControlPosition() turn it into the parameter's own units and back.
    float position = 0.0f;

    // The line to the next point: 0 is straight; toward -maxBend or maxBend it bends one way or the other, still running
    // from this point to the next. Beyond, up to -1 or 1, the value holds at both ends with a shorter bend between them,
    // and at -1 or 1 it holds and then jumps, as discrete parameters' points do.
    float curve = 0.0f;

    static constexpr float maxBend = 0.5f; // the most bending a lane's line gives (engine/automation/Curve.h)
};

// A value in a parameter's own units (hertz, a pan from -1 to 1) as a point's position along its control's travel, and
// back, by the parameter's range as its control spans it, with its skew and steps (ParameterHandle::getRange()): the
// mapping its knob uses. A value outside the range is taken at its end; back, a value is put on the parameter's steps.
float toControlPosition (const juce::NormalisableRange<float>&, float value);
float fromControlPosition (const juce::NormalisableRange<float>&, float position);

// The same by a parameter's travel as the model and the engine hold it (devices::getTravel()), which is the same mapping.
float toControlPosition (const devices::Travel&, float value);
float fromControlPosition (const devices::Travel&, float position);

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

// The points of a shape across a range, swinging between two positions (starting at from): a whole number of cycles, each
// as near the cycle length given as fits.
std::vector<AutomationPoint> makeAutomationShape (AutomationShape, BeatRange, BeatDuration cycle, float from, float to);

//==============================================================================
// Write modes: how moving a track's controls while the song plays writes their automation
// (model/playback/internal/AutomationWriter.h).
enum class AutomationMode
{
    read,  // the lanes play; a control moved by hand goes back to its lane at the lane's next change
    touch, // a control writes while it is held, and goes back to its lane when it is let go
    latch, // a control writes from when it is first moved until playback stops
    write  // from when playback starts, every automated parameter writes its control's value, until the control is let go;
           // then the track is in latch
};
} // namespace phazite
