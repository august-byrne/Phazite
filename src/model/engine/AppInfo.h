// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace phazite
{
// Phazite's name, as used for the settings folder (%APPDATA%\Phazite on Windows) and in project files.
inline constexpr const char* appName = "Phazite";

// Phazite's version, e.g. "0.1.0", from project(VERSION) in CMakeLists.txt.
juce::String getVersionString();

// Phazite's copyright notice, "Copyright (C) 2026 the Phazite contributors", from CMakeLists.txt (PHAZITE_COPYRIGHT), as the
// About box, the installer and the app's version information say it.
juce::String getCopyrightNotice();

// Where Phazite's source code is: its repository on GitHub, from CMakeLists.txt (PHAZITE_SOURCE_URL). Its releases are there
// too (services/Updates.h).
juce::String getSourceCodeAddress();

// The code and fonts from others that Phazite is built with, as this build has them (resources/licenses/notices.json, which
// docs/third-party.md explains), each with the notice its licence asks to go with every copy.
struct ThirdPartyNotice
{
    juce::String name {};      // "Rubber Band Library"
    juce::String version {};   // "4.0", or none
    juce::String licence {};   // as SPDX names it: "GPL-2.0-or-later"
    juce::String copyright {}; // its copyright line
    juce::String use {};       // what Phazite uses it for, a sentence
    juce::String url {};       // its home page
    juce::String file {};      // its notice's text in the licences' folder, or none for what is in the public domain
};

std::vector<ThirdPartyNotice> getThirdPartyNotices();

// What the owners of the trademarks that Phazite names ask it to say, a sentence each (the same list).
juce::StringArray getTrademarkNotices();

// The folder next to the app that holds Phazite's licence (licenceFileName) and the notices' texts, as the build puts them
// there (cmake/PhaziteNotices.cmake), with NOTICES.txt beside it.
juce::File getLicencesFolder();
inline constexpr const char* licenceFileName = "Phazite.txt";
} // namespace phazite
