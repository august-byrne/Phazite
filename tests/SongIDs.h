// SPDX-License-Identifier: AGPL-3.0-or-later
// A song's IDs checked: every item's once in the song (Schema.h: holdsID(); a channel's fader and VCA too), a note's
// once in its clip, and what names an item naming one the song has. For the tests of what puts a song together from
// others: a merge, a version brought back, a line discarded or restored.
#pragma once

#include "TestHelpers.h"
#include "model/base/Song.h"
#include "model/base/internal/Schema.h"

#include <functional>
#include <set>

namespace phazite::test
{
// Every item's ID once in the song, and notes' once in their clips. Returns the items' IDs.
inline std::set<std::uint64_t> checkUniqueIDs (const juce::ValueTree& song)
{
    std::set<std::uint64_t> items;
    const std::function<void (const juce::ValueTree&)> collect = [&] (const juce::ValueTree& tree)
    {
        std::set<std::uint64_t> notes;

        for (const auto& child : tree)
        {
            if (child.hasType (schema::NOTE))
            {
                INFO ("a note's ID twice in its clip");
                CHECK (notes.insert (readID (child[schema::noteID])).second);
                continue;
            }

            for (const auto* property : { &schema::trackID, &schema::channelFader, &schema::channelVCA })
            {
                if (child.hasProperty (*property) && schema::holdsID (child, *property))
                {
                    INFO ("twice in the song: " << (child.getType().toString() + " " + child[*property].toString()).toStdString());
                    CHECK (items.insert (readID (child[*property])).second);
                }
            }

            collect (child);
        }
    };

    collect (song);
    return items;
}

// The same, and what names an item (a sidechain, a lane's, a curve's and a modulation's device, a pad's drum sampler)
// names one the song has; but a send's return, which the song keeps for an undo that brings the return back.
inline void checkIDs (const juce::ValueTree& song)
{
    const auto items = checkUniqueIDs (song);
    const std::function<void (const juce::ValueTree&)> check = [&] (const juce::ValueTree& tree)
    {
        for (const auto& child : tree)
        {
            for (int i = 0; i < child.getNumProperties(); ++i)
            {
                if (const auto property = child.getPropertyName (i); schema::namesID (child, property) && ! child.hasType (schema::SEND))
                {
                    INFO ("names nothing: "
                          << (child.getType().toString() + "@" + property.toString() + " " + child[property].toString()).toStdString());
                    CHECK (items.count (readID (child[property])) == 1);
                }
            }

            check (child);
        }
    };

    check (song);
}

// A song's own, and that the next ID it gives is one nothing in it has.
inline void checkIDs (const Song& song)
{
    checkIDs (song.getState());
    INFO ("the song's count of IDs is behind what it holds");
    CHECK (findNextID (song.getState()) == (std::uint64_t) (juce::int64) song.getState().getProperty (schema::nextID, (juce::int64) 0));
}
} // namespace phazite::test
