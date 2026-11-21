// SPDX-License-Identifier: AGPL-3.0-or-later
// A song's IDs checked: every item's once in the song (Schema.h: holdsID(); a channel's fader and VCA too), a note's
// once in its clip, and what names an item naming one the song has. For the tests of what puts a song together from
// others: a merge, a version brought back, a line discarded or restored; and for the consistency check of a song as it
// loads (SongCheck.h), which takes the problems as lines rather than as failed checks.
#pragma once

#include "TestHelpers.h"
#include "model/base/Song.h"
#include "model/base/internal/Schema.h"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace phazite::test
{
// Whether Phazite knows an element's type (Schema.h: an element it describes). One it doesn't, a later build's or another
// tool's, stays in a song as it is, with everything in it (docs/project-format.md), and nothing reads it: the checks leave
// it and what is in it alone.
inline bool isKnownElementType (const juce::ValueTree& element)
{
    return schema::isKnownType (element.getType());
}

// Each problem with a song's IDs, as a line: "id: ..." for an ID held twice (an item's in the song, a note's in its clip),
// "names: ..." for what names an item the song hasn't (a send's return, a sidechain, a lane's, a curve's and a
// modulation's device, a pad's drum sampler). The items' IDs, if asked.
//
// A song as it is being edited (not asLoaded) keeps more that names what is gone, which opening it leaves out (Project.h:
// makeConsistent()): the curves and lanes of a device taken away, as Schema.h says (AUTOMATION, LANE), so that an undo
// brings them back with it. What else named it goes with it (Mixer.h: forgetWhatNamesTheGone()).
inline std::vector<std::string> findIDProblems (const juce::ValueTree& song, std::set<std::uint64_t>* itemsFound = nullptr,
                                                bool asLoaded = true)
{
    std::vector<std::string> problems;
    std::set<std::uint64_t> items;
    const std::function<void (const juce::ValueTree&)> collect = [&] (const juce::ValueTree& tree)
    {
        std::set<std::uint64_t> notes;

        for (const auto& child : tree)
        {
            if (! isKnownElementType (child))
                continue;

            if (child.hasType (schema::NOTE))
            {
                if (! notes.insert (readID (child[schema::noteID])).second)
                    problems.push_back ("id: a note's ID twice in its clip: " + child[schema::noteID].toString().toStdString());

                continue;
            }

            for (const auto* property : { &schema::trackID, &schema::channelFader, &schema::channelVCA })
                if (child.hasProperty (*property) && schema::holdsID (child, *property))
                    if (! items.insert (readID (child[*property])).second)
                        problems.push_back ("id: twice in the song: "
                                            + (child.getType().toString() + " " + child[*property].toString()).toStdString());

            collect (child);
        }
    };

    collect (song);

    const std::function<void (const juce::ValueTree&)> check = [&] (const juce::ValueTree& tree)
    {
        for (const auto& child : tree)
        {
            if (! isKnownElementType (child))
                continue;

            for (int i = 0; i < child.getNumProperties(); ++i)
                if (const auto property = child.getPropertyName (i);
                    schema::namesID (child, property) && (asLoaded || ! (child.hasType (schema::CURVE) || child.hasType (schema::LANE))))
                    if (items.count (readID (child[property])) != 1)
                        problems.push_back (
                            "names: names nothing: "
                            + (child.getType().toString() + "@" + property.toString() + " " + child[property].toString()).toStdString());

            check (child);
        }
    };

    check (song);

    if (itemsFound != nullptr)
        *itemsFound = std::move (items);

    return problems;
}

// Every item's ID once in the song, and notes' once in their clips. Returns the items' IDs.
inline std::set<std::uint64_t> checkUniqueIDs (const juce::ValueTree& song)
{
    std::set<std::uint64_t> items;

    for (const auto& problem : findIDProblems (song, &items))
    {
        if (problem.rfind ("id:", 0) == 0)
        {
            INFO (problem);
            CHECK (false);
        }
    }

    return items;
}

// The same, and what names an item names one the song has.
inline void checkIDs (const juce::ValueTree& song)
{
    for (const auto& problem : findIDProblems (song, nullptr, false))
    {
        INFO (problem);
        CHECK (false);
    }
}

// A song's own, and that the next ID it gives is one nothing in it has.
inline void checkIDs (const Song& song)
{
    checkIDs (phazite::test::copyWholeSong (song));
    INFO ("the song's count of IDs is behind what it holds");
    CHECK (findNextID (song.getState()) == (std::uint64_t) (juce::int64) song.getState().getProperty (schema::nextID, (juce::int64) 0));
}
} // namespace phazite::test
