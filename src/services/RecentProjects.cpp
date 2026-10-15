// SPDX-License-Identifier: AGPL-3.0-or-later
#include "RecentProjects.h"

namespace phazite
{
RecentProjects::RecentProjects (Settings& s)
    : settings (s)
{
}

void RecentProjects::add (const juce::File& projectFile)
{
    // As RecentlyOpenedFilesList::addFile: the same full path (in the same case) goes, it comes first, and the list
    // keeps the newest maxNumProjects.
    auto paths = settings.get (Settings::recentProjects);
    paths.removeString (projectFile.getFullPathName());
    paths.insert (0, projectFile.getFullPathName());
    paths.removeRange (maxNumProjects, paths.size());
    settings.set (Settings::recentProjects, paths);
}

void RecentProjects::remove (const juce::File& projectFile)
{
    auto paths = settings.get (Settings::recentProjects);
    paths.removeString (projectFile.getFullPathName());
    settings.set (Settings::recentProjects, paths);
}

juce::Array<juce::File> RecentProjects::getProjects() const
{
    // Of the newest maxNumProjects, those that are still there.
    const auto paths = settings.get (Settings::recentProjects);
    juce::Array<juce::File> projects;

    for (int i = 0; i < juce::jmin (paths.size(), maxNumProjects); ++i)
        if (juce::File::isAbsolutePath (paths[i]))
            if (const juce::File project (paths[i]); project.exists())
                projects.add (project);

    return projects;
}
} // namespace phazite
