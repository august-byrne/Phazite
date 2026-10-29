// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
// The strip along the bottom of the window. It says what the control under the pointer does (its
// tooltip, or its description), and otherwise shows the default hint: the guide's next step, or a general tip.
class HintBar : public juce::Component,
                private juce::Timer
{
public:
    HintBar();

    // The hint for when the pointer is on nothing that explains itself, asked for each time the bar looks (ten times a
    // second), so it follows the keys and the guide.
    void setDefaultHint (std::function<juce::String()>);

    juce::String getHint() const { return hint; } // what it shows now
    void paint (juce::Graphics&) override;

    // Something wrong that needs the user, such as no audio output: shown in place of the hints, in the warning color, until
    // cleared (an empty text); a click runs fix.
    void setAlert (const juce::String& text, std::function<void()> fix);
    juce::String getAlert() const { return alert; }

    // A click while no alert shows: the lesson's next step (AppShell, Lessons.h).
    std::function<void()> onClick;

    void mouseUp (const juce::MouseEvent&) override;

private:
    std::function<juce::String()> defaultHint;
    juce::String hint, alert;
    std::function<void()> fixAlert;

    void timerCallback() override; // checks the component under the pointer; repaints only on change
};
} // namespace phazite::ui
