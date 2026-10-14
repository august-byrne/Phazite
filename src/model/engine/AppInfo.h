// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_core/juce_core.h>

namespace phazite
{
// Phazite's name, as used for the settings folder (%APPDATA%\Phazite on Windows) and in project files.
inline constexpr const char* appName = "Phazite";

// Phazite's version, e.g. "0.1.0", from project(VERSION) in CMakeLists.txt.
juce::String getVersionString();
} // namespace phazite
