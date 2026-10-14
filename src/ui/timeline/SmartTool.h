// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist's tools (docs/design.md, section 6). The Smart Tool is the default: what dragging a clip
// does depends on where the drag starts, as with Pro Tools' Smart Tool. The others do one thing each and
// are picked with FL Studio's letters; Esc returns to the Smart Tool.
#pragma once

#include <juce_graphics/juce_graphics.h>

namespace phazite::ui
{
enum class Tool
{
    smart,
    draw,   // P: a click places a copy of the last clip used
    paint,  // B: dragging places copies of it, one after another
    slice,  // C: a click splits a clip
    slip,   // S: dragging moves what is inside a clip, leaving the clip where it is
    mute,   // T: a click mutes or unmutes a clip
    erase,  // D: a click deletes a clip, and so does dragging over clips
    select, // E: dragging selects the clips a rectangle touches
    zoom    // Z: a click zooms in, an Alt-click out, and dragging zooms to the dragged range
};

inline constexpr Tool allTools[] = { Tool::smart, Tool::draw,  Tool::paint,  Tool::slice, Tool::slip,
                                     Tool::mute,  Tool::erase, Tool::select, Tool::zoom };

juce::String getToolName (Tool);      // "Draw", for menus and buttons
juce::String getToolCommandID (Tool); // "tool.draw"
juce::String describeTool (Tool);     // what it does, for the hint bar

//==============================================================================
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
} // namespace phazite::ui
