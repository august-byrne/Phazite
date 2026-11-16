// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Song.h"
#include "internal/SongIndex.h"

#include <atomic>

namespace phazite
{
namespace
{
    // Set on the message thread, read wherever a song is made: the checks of a song read from the versions make one on the
    // history's thread (model/versions/SongPieces.cpp).
    std::atomic<int> undoSteps { Song::defaultUndoSteps };
} // namespace

void Song::setUndoSteps (int steps)
{
    undoSteps = juce::jlimit (minimumUndoSteps, maximumUndoSteps, steps);
}

int Song::getUndoSteps()
{
    return undoSteps;
}

Song::Song (juce::ValueTree s)
    : state (std::move (s)),
      index (std::make_unique<SongIndex> (state))
{
    // By steps alone: JUCE weighs each change by the size of its record, not by what it holds, so a budget of units kept as
    // few as 30 steps after a drag of many notes.
    undoManager.setMaxNumberOfStoredUnits (1, undoSteps.load());
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

std::map<std::uint64_t, int> Song::getDeviceChangesHeld() const
{
    auto counts = deviceChangesOutsideHistory;

    for (const auto& [device, held] : deviceChangesHeld)
        if (const auto found = counts.find (device); found != counts.end())
            found->second = std::min (found->second, held);

    return counts;
}
} // namespace phazite
