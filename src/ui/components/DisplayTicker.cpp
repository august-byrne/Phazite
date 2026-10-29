// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DisplayTicker.h"

#include <algorithm>

namespace phazite::ui
{
DisplayTicker::Display::Display (juce::Component& shown)
    : shownBy (shown)
{
    ticker->add (*this);
}

DisplayTicker::Display::~Display()
{
    ticker->remove (*this);
}

//==============================================================================
DisplayTicker::~DisplayTicker()
{
    stopTimer();
}

void DisplayTicker::add (Display& display)
{
    displays.push_back (&display);

    if (! isTimerRunning())
        startTimerHz (ticksPerSecond);
}

void DisplayTicker::remove (Display& display)
{
    displays.erase (std::remove (displays.begin(), displays.end(), &display), displays.end());

    if (displays.empty())
        stopTimer();
}

void DisplayTicker::tickNow()
{
    // A tick may add or remove displays (an editor that rebuilds a part of itself): each is looked for again before it ticks.
    const auto ticking = displays;

    for (auto* display : ticking)
        if (std::find (displays.begin(), displays.end(), display) != displays.end() && display->shownBy.isShowing())
            display->tick();
}

//==============================================================================
bool DrawnNumbers::changed (std::vector<float> numbers)
{
    const auto same =
        numbers.size() == last.size()
        && std::equal (numbers.begin(), numbers.end(), last.begin(), [] (float a, float b) { return juce::exactlyEqual (a, b); });
    last = std::move (numbers);
    return ! same;
}
} // namespace phazite::ui
