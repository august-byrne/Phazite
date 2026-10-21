// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceTicker.h"

#include <algorithm>

namespace phazite::ui
{
DeviceTicker::Display::Display (juce::Component& shownBy)
    : component (shownBy)
{
    ticker->add (*this);
}

DeviceTicker::Display::~Display()
{
    ticker->remove (*this);
}

//==============================================================================
DeviceTicker::~DeviceTicker()
{
    stopTimer();
}

void DeviceTicker::add (Display& display)
{
    displays.push_back (&display);

    if (! isTimerRunning())
        startTimerHz (ticksPerSecond);
}

void DeviceTicker::remove (Display& display)
{
    displays.erase (std::remove (displays.begin(), displays.end(), &display), displays.end());

    if (displays.empty())
        stopTimer();
}

void DeviceTicker::tickNow()
{
    // A tick may add or remove displays (an editor that rebuilds a part of itself): each is looked for again before it ticks.
    const auto ticking = displays;

    for (auto* display : ticking)
        if (std::find (displays.begin(), displays.end(), display) != displays.end() && display->component.isShowing())
            display->tick();
}
} // namespace phazite::ui
