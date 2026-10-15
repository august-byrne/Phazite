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

private:
    std::function<juce::String()> defaultHint;
    juce::String hint;

    void timerCallback() override; // checks the component under the pointer; repaints only on change
};
} // namespace phazite::ui
