// SPDX-License-Identifier: AGPL-3.0-or-later
#include "FrameLog.h"

#include <algorithm>
#include <cmath>

namespace phazite::ui
{
void FrameLog::frame (double vblankMilliseconds, double takenMilliseconds, double beat, double drawnX, bool moved)
{
    // The last frame's move, still unpainted as this frame comes: JUCE paints it at a later vertical blank, if at all.
    if (waitingForPaint)
        ++paintedLate;

    waitingForPaint = moved;
    movedAt = vblankMilliseconds;

    const auto late = std::max (0.0, takenMilliseconds - vblankMilliseconds);
    lateSum += late;
    mostLate = std::max (mostLate, late);

    if (frames == 0)
    {
        firstVblank = vblankMilliseconds;
    }
    else if (const auto interval = vblankMilliseconds - lastVblank; interval > 0.0)
    {
        // The display's frame is the shortest time between two; a longer one held vertical blanks the thread missed.
        shortest = shortest > 0.0 ? std::min (shortest, interval) : interval;
        missed += std::max (0, juce::roundToInt (interval / shortest) - 1);

        // How evenly the line moves: its speed against the last frame's, over a frame. A jump starts again.
        const auto step = drawnX - lastX;

        if (std::abs (step) < jump && beat >= lastBeat)
        {
            if (beat > lastBeat && std::abs (step) < 1.0e-9)
                ++stoodStill;

            const auto speed = step / interval;

            if (hasSpeed)
            {
                const auto change = std::abs (speed - lastSpeed) * shortest;
                changeSum += change;
                largestChange = std::max (largestChange, change);
                ++steps;
            }

            lastSpeed = speed;
            hasSpeed = true;
        }
        else
        {
            hasSpeed = false;
        }
    }

    ++frames;
    lastVblank = vblankMilliseconds;
    lastBeat = beat;
    lastX = drawnX;
}

void FrameLog::painted (double milliseconds)
{
    if (waitingForPaint)
        longestPaintWait = std::max (longestPaintWait, milliseconds - movedAt);

    waitingForPaint = false;
}

FrameLog::Summary FrameLog::getSummary() const
{
    Summary summary;
    summary.frames = frames;
    summary.seconds = (lastVblank - firstVblank) / 1000.0;
    summary.frameMilliseconds = shortest;
    summary.meanLate = frames > 0 ? lateSum / frames : 0.0;
    summary.mostLate = mostLate;
    summary.missed = missed;
    summary.longestPaintWait = longestPaintWait;
    summary.paintedLate = paintedLate;
    summary.stoodStill = stoodStill;
    summary.meanStepChange = steps > 0 ? changeSum / steps : 0.0;
    summary.largestStepChange = largestChange;
    return summary;
}

juce::String FrameLog::takeSummary (const juce::String& editor, const juce::String& following)
{
    const auto s = getSummary();
    *this = FrameLog();

    const auto ms = [] (double value) { return juce::String (value, 1) + " ms"; };
    const auto pixels = [] (double value) { return juce::String (value, 2) + " pixels"; };

    return "Frames, " + editor + " (" + following + "): " + juce::String (s.frames) + " in " + juce::String (s.seconds, 1)
           + " s of playing, one every " + ms (s.frameMilliseconds) + "; taken " + ms (s.meanLate)
           + " after their vertical blank on average and " + ms (s.mostLate) + " at most, " + juce::String (s.missed)
           + " missed; a move waited " + ms (s.longestPaintWait) + " at most for its paint, and " + juce::String (s.paintedLate)
           + " waited for a later frame; the playhead stood still in " + juce::String (s.stoodStill)
           + " frames while the song played on, and its step changed by " + pixels (s.meanStepChange) + " a frame on average and "
           + pixels (s.largestStepChange) + " at most.";
}
} // namespace phazite::ui
