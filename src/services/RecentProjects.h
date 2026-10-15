// SPDX-License-Identifier: AGPL-3.0-or-later
// The projects opened most recently, newest first, kept in Phazite's settings in the format JUCE's
// RecentlyOpenedFilesList uses: the full paths, one per line.
#pragma once

#include "Settings.h"

namespace phazite
{
class RecentProjects
{
public:
    static constexpr int maxNumProjects = 10;

    explicit RecentProjects (Settings&);

    void add (const juce::File& projectFile); // first, once
    void remove (const juce::File& projectFile);
    juce::Array<juce::File> getProjects() const; // only those that still exist

private:
    Settings& settings;
};
} // namespace phazite
