// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Song.h"
#include "internal/SongIndex.h"
#include "internal/SongValues.h"

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
    undoManager.stepsKept = undoSteps.load();
    undoManager.keepMoreSteps (0);
}

Song::~Song()
{
    values = nullptr; // its keeper listens to the tree

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

void Song::tellValueSetByProgram (const ParameterRef& parameter, float value)
{
    parameterListeners.call ([&] (ParameterListener& l) { l.valueSetByProgram (parameter, value); });
}

void Song::setDeviceStateStore (std::function<void (std::uint64_t)> store)
{
    deviceStateStore = std::move (store);
}

void Song::storeDeviceStates (std::uint64_t trackID)
{
    writePending();

    if (deviceStateStore != nullptr)
        deviceStateStore (trackID);
}

void Song::addPendingWriter (PendingWriter* writer)
{
    JUCE_ASSERT_MESSAGE_THREAD
    pendingWriters.add (writer);
}

void Song::removePendingWriter (PendingWriter* writer)
{
    JUCE_ASSERT_MESSAGE_THREAD
    pendingWriters.remove (writer);
}

void Song::writePending()
{
    // None but on the message thread: a song read or checked on another thread (model/versions/SongPieces.cpp) has none.
    pendingWriters.call ([] (PendingWriter& writer) { writer.writePending(); });
}

void Song::setShowingOther (bool showing)
{
    if (showing == showingOther)
        return;

    if (showingHeard != nullptr)
        showingHeard (showing);

    showingOther = showing;

    // The step that shows the other, and one begun on top of it (an edit, which may be taken back), kept beside every step of
    // its own, so that neither takes one of them away as it begins; more as more begin on top of it (Trial::stepBegins()). JUCE drops steps only as one is performed, never as the
    // limit falls (JUCE workaround W56 (docs/juce-workarounds.md)).
    undoManager.keepMoreSteps (showing ? 2 : 0);
}

Song::OutsideHistory::OutsideHistory (Song& s)
    : song (s)
{
    JUCE_ASSERT_MESSAGE_THREAD
    ++song.writingOutsideHistory;
}

Song::OutsideHistory::~OutsideHistory()
{
    jassert (song.writingOutsideHistory > 0);
    --song.writingOutsideHistory;
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
