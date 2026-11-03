// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Song.h"

namespace phazite
{
Song::Song (juce::ValueTree s)
    : state (std::move (s))
{
    undoManager.setMaxNumberOfStoredUnits (30000, 30);
}

Song::~Song()
{
    if (ownCacheFolder != juce::File())
        ownCacheFolder.deleteRecursively();
}

void Song::setCacheFolder (const juce::File& folder)
{
    cacheFolder = folder;
}

juce::File Song::getCacheFolder() const
{
    if (cacheFolder != juce::File())
        return cacheFolder;

    if (ownCacheFolder == juce::File())
    {
        ownCacheFolder =
            juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Phazite").getNonexistentChildFile ("Song", {}, false);
        ownCacheFolder.createDirectory();
    }

    return ownCacheFolder;
}

void Song::setTracktionIDs (std::function<std::uint64_t()> takeNext)
{
    takeTracktionID = std::move (takeNext);
}

void Song::setMirror (std::function<void (Part, std::uint64_t)> reconcile)
{
    mirror = std::move (reconcile);
}

void Song::reconcile (Part part, std::uint64_t id)
{
    if (mirror != nullptr)
        mirror (part, id);
}

void Song::setDeviceStateStore (std::function<void (std::uint64_t)> store)
{
    deviceStateStore = std::move (store);
}

void Song::storeDeviceStates (std::uint64_t trackID)
{
    if (deviceStateStore != nullptr)
        deviceStateStore (trackID);
}
} // namespace phazite
