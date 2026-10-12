// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SmartTool.h"

namespace phazite
{
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
