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

void Song::addParameterListener (ParameterListener* listener)
{
    parameterListeners.add (listener);
}

void Song::removeParameterListener (ParameterListener* listener)
{
    parameterListeners.remove (listener);
}

void Song::tellValueSetByHand (const ParameterRef& parameter, float value, bool byPlugin)
{
    parameterListeners.call ([&] (ParameterListener& l) { l.valueSetByHand (parameter, value, byPlugin); });
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
