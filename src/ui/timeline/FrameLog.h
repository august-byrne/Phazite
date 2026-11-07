// SPDX-License-Identifier: AGPL-3.0-or-later
// What the display's frames did while the song played, in an editor that shows the playhead, for Phazite's log once the
// song stops (docs/performance-audit.md, section 16): how late the message thread took them after their vertical blank and
// how many it missed; how long a move of the playhead or the view waited for its paint, and how many waited until a later
// frame (JUCE's Direct2D window paints at a vertical blank only once its swap chain is ready); and how evenly the playhead
// moved: frames in which it stood still while the song played on, and how much its step changed from one frame to the next.
#pragma once

#include <juce_core/juce_core.h>

namespace phazite::ui
{
class FrameLog
{
public:
    // A frame: when its vertical blank was and when the message thread took it, in milliseconds by
    // juce::Time::getMillisecondCounterHiRes(); the beat the playhead showed; where its line was drawn on the song's time line
    // (the view's start and the line's place in the view, in pixels at the editor's zoom); and whether the frame moved what
    // shows (the playhead or the view), which a paint must then show.
    void frame (double vblankMilliseconds, double takenMilliseconds, double beat, double drawnX, bool moved);

    // The playhead's area was painted, at a time by the same clock.
    void painted (double milliseconds);

    struct Summary
    {
        int frames = 0;
        double seconds = 0.0;                                 // from the first frame to the last
        double frameMilliseconds = 0.0;                       // the display's frame: the shortest time between two
        double meanLate = 0.0, mostLate = 0.0;                // the message thread's delay after a vertical blank, in milliseconds
        int missed = 0;                                       // vertical blanks it took none of
        double longestPaintWait = 0.0;                        // from a move's vertical blank to its paint, in milliseconds
        int paintedLate = 0;                                  // moves still unpainted as the next frame came
        int stoodStill = 0;                                   // frames in which the line didn't move while the song played on
        double meanStepChange = 0.0, largestStepChange = 0.0; // pixels a frame
    };

    Summary getSummary() const;
    int getFrames() const noexcept { return frames; }

    // The summary in a sentence for the log, about an editor and how it followed the playhead; nothing is kept after it.
    juce::String takeSummary (const juce::String& editor, const juce::String& following);

    // A step this many pixels from the last one is a jump (a loop's end, a locate, a zoom), not a step.
    static constexpr double jump = 64.0;

private:
    int frames = 0, missed = 0, paintedLate = 0, stoodStill = 0, steps = 0;
    double firstVblank = 0.0, lastVblank = 0.0, shortest = 0.0;
    double lateSum = 0.0, mostLate = 0.0;
    double lastBeat = 0.0, lastX = 0.0, lastSpeed = 0.0; // pixels a millisecond
    bool hasSpeed = false;
    double changeSum = 0.0, largestChange = 0.0;
    bool waitingForPaint = false;
    double movedAt = 0.0, longestPaintWait = 0.0; // the vertical blank of the move waiting for its paint
};
} // namespace phazite::ui
