// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "model/AppInfo.h"

TEST_CASE ("The version string comes from CMake's project version")
{
    CHECK (phazite::getVersionString().isNotEmpty());
    CHECK (phazite::getVersionString() == juce::String (PHAZITE_VERSION_STRING));
    CHECK (phazite::getVersionString().matchesWildcard ("*.*.*", true));
}
