// SPDX-License-Identifier: AGPL-3.0-or-later
// One timer for the displays of every device editor (their meters, curves, scopes and knobs' values), rather than one each
// (docs/performance-audit.md, U5): it ticks 30 times a second, and only the displays that show, so that an editor behind
// another tab, in a folded card or in a closed window costs nothing.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

namespace phazite::ui
{
class DeviceTicker : private juce::Timer
{
public:
    static constexpr int ticksPerSecond = 30;

    // A display that the ticker brings up to date: a component, ticked while it shows.
    class Display
    {
    public:
        explicit Display (juce::Component& shownBy);
        virtual ~Display();

        // Brings what it shows up to date; called while its component shows.
        virtual void tick() = 0;

    private:
        juce::Component& component;
        juce::SharedResourcePointer<DeviceTicker> ticker;

        friend class DeviceTicker;
        JUCE_DECLARE_NON_COPYABLE (Display)
    };

    ~DeviceTicker() override;

    // Ticks every display that shows now; the timer does it ticksPerSecond times a second. For tests.
    void tickNow();

private:
    std::vector<Display*> displays;

    void add (Display&);
    void remove (Display&);
    void timerCallback() override { tickNow(); }
};
} // namespace phazite::ui
