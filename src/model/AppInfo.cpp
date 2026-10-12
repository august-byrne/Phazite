// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AppInfo.h"

namespace phazite
{
juce::String getVersionString()
{
    return PHAZITE_VERSION_STRING; // defined by CMakeLists.txt
}
} // namespace phazite
