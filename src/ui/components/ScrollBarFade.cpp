// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ScrollBarFade.h"
#include "ui/theme/DesignTokens.h"

#include <algorithm>

namespace phazite::ui
{
ScrollBarFade::ScrollBarFade (juce::ScrollBar& scrollBar, juce::Component& over)
    : bar (scrollBar),
      editor (over)
{
    bar.setAlpha (0.0f);
    editor.addMouseListener (this, true); // over its children too, the bar among them
}

ScrollBarFade::~ScrollBarFade()
{
    editor.removeMouseListener (this);
}

void ScrollBarFade::show()
{
    lastWanted = juce::Time::getMillisecondCounterHiRes();

    // In, a frame at a time; or, shown already, looked at now and then for when to fade out. A timer already running at that
    // rate runs on: started again, it would put its next step off, and the pointer moves more often than a frame, so the bar
    // showed only once the pointer stopped (found 2026-11-07).
    if (target < 1.0f || bar.getAlpha() < 1.0f)
    {
        target = 1.0f;

        if (getTimerInterval() != 1000 / ScrollTint::fadeFramesPerSecond)
            startTimerHz (ScrollTint::fadeFramesPerSecond);
    }
    else if (! isTimerRunning())
    {
        startTimerHz (ScrollTint::fadeChecksPerSecond);
    }
}

void ScrollBarFade::timerCallback()
{
    // Wanted while the pointer is over the editor, or drags the bar, and for a moment after.
    const auto now = juce::Time::getMillisecondCounterHiRes();

    if (editor.isMouseOverOrDragging (true))
        lastWanted = now;

    target = now - lastWanted < ScrollTint::fadeHoldMs ? 1.0f : 0.0f;

    // A frame's step towards it: in quickly, out more slowly.
    const auto alpha = bar.getAlpha();
    const auto fadeMs = target > alpha ? ScrollTint::fadeInMs : ScrollTint::fadeOutMs;
    const auto step = 1.0f / std::max (1.0f, (float) (fadeMs * ScrollTint::fadeFramesPerSecond / 1000.0));
    const auto next = target > alpha ? std::min (target, alpha + step) : std::max (target, alpha - step);

    if (! juce::exactlyEqual (next, alpha))
        bar.setAlpha (next);

    // Fading: a frame at a time. Shown: looked at now and then. Faded: nothing until it is wanted again.
    const auto every = [this] (int hertz)
    {
        if (getTimerInterval() != 1000 / hertz)
            startTimerHz (hertz);
    };

    if (! juce::exactlyEqual (next, target))
        every (ScrollTint::fadeFramesPerSecond);
    else if (target > 0.0f)
        every (ScrollTint::fadeChecksPerSecond);
    else
        stopTimer();
}
} // namespace phazite::ui
