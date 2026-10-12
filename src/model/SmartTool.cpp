// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SmartTool.h"

namespace phazite
{
juce::String getToolName (Tool tool)
{
    switch (tool)
    {
        case Tool::draw:
            return "Draw";
        case Tool::paint:
            return "Paint";
        case Tool::slice:
            return "Slice";
        case Tool::slip:
            return "Slip";
        case Tool::mute:
            return "Mute";
        case Tool::erase:
            return "Delete";
        case Tool::select:
            return "Select";
        case Tool::zoom:
            return "Zoom";
        case Tool::smart:
        default:
            return "Smart Tool";
    }
}

juce::String getToolCommandID (Tool tool)
{
    switch (tool)
    {
        case Tool::draw:
            return "tool.draw";
        case Tool::paint:
            return "tool.paint";
        case Tool::slice:
            return "tool.slice";
        case Tool::slip:
            return "tool.slip";
        case Tool::mute:
            return "tool.mute";
        case Tool::erase:
            return "tool.delete";
        case Tool::select:
            return "tool.select";
        case Tool::zoom:
            return "tool.zoom";
        case Tool::smart:
        default:
            return "tool.smart";
    }
}

juce::String describeTool (Tool tool)
{
    switch (tool)
    {
        case Tool::draw:
            return "Click to place a copy of the last clip you used; on a track with an instrument and no clip used yet, a new MIDI clip.";
        case Tool::paint:
            return "Drag to place copies of the last clip you used, one after another.";
        case Tool::slice:
            return "Click a clip to split it there. Hold Alt to split off the grid.";
        case Tool::slip:
            return "Drag a clip to move what is inside it, leaving the clip where it is.";
        case Tool::mute:
            return "Click a clip to mute or unmute it; drag to do it to several.";
        case Tool::erase:
            return "Click a clip to delete it; drag over clips to delete them all.";
        case Tool::select:
            return "Drag to select the clips in a rectangle; with Ctrl, add them to the selection.";
        case Tool::zoom:
            return "Click to zoom in, Alt-click to zoom out, or drag across the time to zoom to.";
        case Tool::smart:
        default:
            return "Where you drag a clip decides what happens: edges trim, top corners fade, the upper half selects time, "
                   "the lower half moves.";
    }
}

ClipZone getClipZone (juce::Rectangle<float> bounds, juce::Point<float> point, bool hasFades)
{
    const auto x = point.x - bounds.getX(), y = point.y - bounds.getY();
    const auto width = bounds.getWidth(), height = bounds.getHeight();

    // Narrow clips keep most of their width for moving.
    const auto edge = std::min (6.0f, width / 4.0f);
    const auto corner = std::min (12.0f, width / 3.0f);

    if (hasFades && y < std::min (10.0f, height / 3.0f))
    {
        if (x < corner)
            return ClipZone::fadeIn;

        if (x > width - corner)
            return ClipZone::fadeOut;
    }

    if (x < edge)
        return ClipZone::trimStart;

    if (x > width - edge)
        return ClipZone::trimEnd;

    return y < height / 2.0f ? ClipZone::selectRange : ClipZone::move;
}

juce::String describeClipZone (ClipZone zone)
{
    switch (zone)
    {
        case ClipZone::trimStart:
            return "Drag to trim the start of the clip. Hold Alt to place it freely.";
        case ClipZone::trimEnd:
            return "Drag to trim the end of the clip. Hold Alt to place it freely.";
        case ClipZone::fadeIn:
            return "Drag to fade the clip in.";
        case ClipZone::fadeOut:
            return "Drag to fade the clip out.";
        case ClipZone::selectRange:
            return "Click to select the clip; drag to select a time range. Ctrl+click adds to the selection.";
        case ClipZone::move:
        default:
            return "Drag to move the clip; with Ctrl held, to copy it, and with Ctrl and Shift, to make an alias. "
                   "Hold Alt to place it freely.";
    }
}
} // namespace phazite
