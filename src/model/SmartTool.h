// SPDX-License-Identifier: AGPL-3.0-or-later
// The Smart Tool, the playlist's pointer (docs/design.md, section 6): what dragging a clip does depends on
// where the drag starts, as with Pro Tools' Smart Tool.
#pragma once

#include <juce_graphics/juce_graphics.h>

namespace phazite
{
enum class ClipZone
{
    trimStart,   // the left edge
    trimEnd,     // the right edge
    fadeIn,      // the top left corner of an audio clip
    fadeOut,     // the top right corner of an audio clip
    selectRange, // the upper half: selects a time range
    move         // the lower half: moves the clip, or copies it
};

// The zone of a clip a point is in (the clip's bounds and the point in the same coordinates). Only
// audio clips have fades.
ClipZone getClipZone (juce::Rectangle<float> clipBounds, juce::Point<float>, bool hasFades);

// What dragging from a zone does, for the hint bar.
juce::String describeClipZone (ClipZone);
} // namespace phazite
