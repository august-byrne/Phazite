// SPDX-License-Identifier: AGPL-3.0-or-later
// A scroll bar that lies over what it scrolls (the rows' of the playlist and the piano roll), shown while it is wanted and
// faded out after, as macOS's and Windows 11's overlay scroll bars are (the owner, 2026-11-07: they should "fade away when
// the cursor is not hovering on the playlist for a while"): while the pointer is over its editor, the bar among it (which
// takes in scrolling there with the wheel and dragging the bar), and for a moment after (ScrollTint::fadeHoldMs); not for
// the editor scrolling itself, so a project that opens doesn't flash it. Faded out, it stays where it is and still takes
// clicks; nothing repaints while it rests, shown or faded.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
class ScrollBarFade : private juce::MouseListener,
                      private juce::Timer
{
public:
    // A scroll bar, faded out until it is wanted, and the editor it lies over, which the pointer shows it over. Both outlive
    // the fade.
    ScrollBarFade (juce::ScrollBar&, juce::Component& editor);
    ~ScrollBarFade() override;

    bool isShown() const noexcept { return target > 0.0f; } // shown, or on its way in

private:
    juce::ScrollBar& bar;
    juce::Component& editor;
    double lastWanted = 0.0; // when it was wanted last (juce::Time::getMillisecondCounterHiRes())
    float target = 0.0f;     // the alpha it fades to: 1 while it is wanted, 0 after

    void show(); // wanted now: fades in, or stays

    void mouseEnter (const juce::MouseEvent&) override { show(); }
    void mouseMove (const juce::MouseEvent&) override { show(); }
    void mouseDrag (const juce::MouseEvent&) override { show(); }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override { show(); }
    void timerCallback() override;
};
} // namespace phazite::ui
