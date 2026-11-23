// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestTracks.h"

#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/WholeState.h"

#include <functional>

namespace phazite::test
{
juce::ValueTree findTrackIn (const juce::ValueTree& wholeSong, TrackID id)
{
    if (! id.isValid())
        return {};

    if (const auto master = wholeSong.getChildWithName (schema::MASTER); readID (master[schema::trackID]) == id.getRaw())
        return master;

    const std::function<juce::ValueTree (const juce::ValueTree&)> findIn = [&] (const juce::ValueTree& list) -> juce::ValueTree
    {
        for (const auto& child : list)
        {
            if (! child.hasType (schema::TRACK))
                continue;

            if (readID (child[schema::trackID]) == id.getRaw())
                return child;

            if (const auto found = findIn (child); found.isValid())
                return found;
        }

        return {};
    };

    return findIn (wholeSong.getChildWithName (schema::TRACKS));
}

juce::ValueTree copyTrack (const Song& song, TrackID id)
{
    return findTrackIn (phazite::test::copyWholeSong (song), id);
}

bool hasTrack (const Song& song, TrackID id)
{
    return phazite::hasTrack (song, id);
}

juce::var getTrackProperty (const Song& song, TrackID id, const juce::Identifier& property)
{
    return copyTrack (song, id)[property];
}

bool treeHolds (const Song& song, const juce::Identifier& type)
{
    const std::function<bool (const juce::ValueTree&)> holds = [&] (const juce::ValueTree& element)
    {
        for (const auto& child : element)
            if (child.hasType (type) || holds (child))
                return true;

        return false;
    };

    return holds (song.getState());
}
} // namespace phazite::test
