// SPDX-License-Identifier: AGPL-3.0-or-later
// One timer for every display that changes while it shows (level meters, and device editors' meters, curves, scopes and
// knobs' values), rather than one each (docs/performance-audit.md, U5 and section 8): it ticks 30 times a second, and only
// the displays that show, so that one behind another tab, in a folded card or in a closed window costs nothing; and what
// they repaint is painted together, in one pass of the window.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

namespace phazite::ui
{
class DisplayTicker : private juce::Timer
{
public:
    static constexpr int ticksPerSecond = 30;

    // A display that the ticker brings up to date: a component, ticked while it shows.
    class Display
    {
    public:
        explicit Display (juce::Component& shown);
        virtual ~Display();

        // Brings what it shows up to date, repainting what changed; called while its component shows.
        virtual void tick() = 0;

    private:
        juce::Component& shownBy;
        juce::SharedResourcePointer<DisplayTicker> ticker;

        friend class DisplayTicker;
        JUCE_DECLARE_NON_COPYABLE (Display)
    };

    ~DisplayTicker() override;

    // Ticks every display that shows now; the timer does it ticksPerSecond times a second. For tests.
    void tickNow();

private:
    std::vector<Display*> displays;

    void add (Display&);
    void remove (Display&);
    void timerCallback() override { tickNow(); }
};

// The numbers a display draws from, so that its tick() repaints it only when they change (docs/performance-audit.md,
// section 8): the tick gathers them (its parameters' values, where an LFO is, levels, a history's readings) and repaints if
// they differ from the last tick's. In silence, with nothing turned, a display then repaints nothing.
class DrawnNumbers
{
public:
    // Keeps these numbers in place of the last tick's; true if they differ.
    bool changed (std::vector<float> numbers);

private:
    std::vector<float> last;
};
} // namespace phazite::ui
