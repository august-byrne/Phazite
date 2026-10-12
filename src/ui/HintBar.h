// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
// The strip along the bottom of the window. It says what the control under the pointer does (its
// tooltip, or its description), and otherwise shows a general tip.
class HintBar : public juce::Component,
                private juce::Timer
{
public:
    HintBar();

    void setDefaultHint (const juce::String&);
    void paint (juce::Graphics&) override;

private:
    juce::String defaultHint, hint;

    void timerCallback() override; // checks the component under the pointer; repaints only on change
};
} // namespace phazite::ui
