// SPDX-License-Identifier: AGPL-3.0-or-later
// A switch for several things at once (src/model/base/Toggles.h).
#include "TestHelpers.h"
#include "model/base/Toggles.h"

#include <vector>

TEST_CASE ("A switch for several things turns them all on, unless all are on")
{
    bool a = false, b = true, c = false;
    const std::vector<bool*> items { &a, &b, &c };
    const auto isOn = [] (bool* item) { return *item; };
    const auto setOn = [] (bool* item, bool on) { *item = on; };

    // Some on: all go on.
    CHECK_FALSE (phazite::areAllOn (items, isOn));
    CHECK (phazite::toggleAll (items, isOn, setOn));
    CHECK ((a && b && c));

    // All on: all go off.
    CHECK (phazite::areAllOn (items, isOn));
    CHECK_FALSE (phazite::toggleAll (items, isOn, setOn));
    CHECK_FALSE ((a || b || c));

    // None on: all go on.
    CHECK (phazite::toggleAll (items, isOn, setOn));
    CHECK ((a && b && c));

    // Nothing at all isn't "all on", and switching it changes nothing.
    const std::vector<bool*> none;
    CHECK_FALSE (phazite::areAllOn (none, isOn));
    CHECK (phazite::toggleAll (none, isOn, setOn));
}
