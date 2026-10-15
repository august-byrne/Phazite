// SPDX-License-Identifier: AGPL-3.0-or-later
// A switch for several things at once, as the mute, solo, collapse and pin commands have: it turns them all on,
// unless all of them are on already, and then turns them all off.
#pragma once

#include <algorithm>
#include <iterator>

namespace phazite
{
// Whether there are items and all of them are on.
template <typename Items, typename IsOn>
bool areAllOn (const Items& items, IsOn isOn)
{
    return std::begin (items) != std::end (items) && std::all_of (std::begin (items), std::end (items), isOn);
}

// Turns every item on, or every one off if all of them are on. Returns whether they are on now.
template <typename Items, typename IsOn, typename SetOn>
bool toggleAll (const Items& items, IsOn isOn, SetOn setOn)
{
    const auto turnOn = ! areAllOn (items, isOn);

    for (const auto& item : items)
        setOn (item, turnOn);

    return turnOn;
}
} // namespace phazite
