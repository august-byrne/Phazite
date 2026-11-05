// SPDX-License-Identifier: AGPL-3.0-or-later
// Two lines of a song merged (docs/versions.md: Merging by IDs): what the other side changed since the newest version both
// have, taken into this side's, a line at a time (a track, the master, the song's own), each element by its key
// (model/base/internal/Schema.h: getElementKey()), each property on its own. A property changed on one side takes that
// side's value; changed on both to different values, it is a clash, which keeps this side's value and all three. An element
// added on one side is added; taken away on one side and unchanged on the other, it goes; taken away on one side and
// changed on the other, it stays, and a note says so. Order takes the other side's moves where this side made none. What
// the other side added keeps its IDs where this side has nothing with them, and gets new ones where this side has
// something else with them (giveNewIDs()'s lists: holdsID(), namesID()), its notes in clips both have too: no ID collides.
// Curves, the chord lane and a plug-in's state merge whole; view state and how the song plays are always this side's. Plain
// functions of trees, for any thread, but for the IDs.
#pragma once

#include "SongDiff.h"

#include <juce_data_structures/juce_data_structures.h>

#include <functional>
#include <set>
#include <vector>

namespace phazite
{
// A change both sides made, to different values: this side's to start with, with all three.
struct MergeClash
{
    juce::String line;            // the line it is on: a track's name, "Master" or "Song"
    std::uint64_t trackID = 0;    // that track's (or the master's) ID, 0 for the song's own
    juce::StringArray path;       // the element it is in, from the song down: each element's type and key ("TRACK:12")
    juce::Identifier property;    // its property, or none for the element as a whole (a curve, the chord lane)
    juce::var base, ours, theirs; // the property's three values, void where it had none
    juce::ValueTree baseElement, oursElement, theirsElement; // the element's, invalid where there was none
    juce::String what;                                       // in Phazite's own words: "volume", "Synth: cutoff", "automation"
};

struct MergeResult
{
    juce::ValueTree song; // this side's, with the other's lines taken in
    std::vector<MergeClash> clashes;
    juce::StringArray notes; // what a merge can't decide for the user, such as "Bass: changed here, taken away there: kept"
};

// What a merge takes, of the lines the other side changed since the two parted (findChanges (base, theirs)): tracks by their
// IDs (the other side's), the master by its ID, and the song's own.
struct MergeLines
{
    std::set<std::uint64_t> tracks;
    bool songOwn = false;
};

// Merges the other side's lines into a copy of this side's song. New IDs come from makeID (the song's: Song::makeID()).
MergeResult mergeSongs (const juce::ValueTree& base, const juce::ValueTree& ours, const juce::ValueTree& theirs, const MergeLines&,
                        const std::function<std::uint64_t()>& makeID);

// The element a clash's path names in a song, invalid if it has none.
juce::ValueTree findByPath (const juce::ValueTree& song, const juce::StringArray& path);
} // namespace phazite
