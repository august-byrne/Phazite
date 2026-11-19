// SPDX-License-Identifier: AGPL-3.0-or-later
// Two lines of a song merged (docs/design/versions.md: Merging): everything the other side changed since the newest version both
// have, taken into this side's, a line at a time (a track, the master, the song's own), each element by its key
// (model/base/internal/Schema.h: getElementKey()), each property on its own. A property changed on one side takes that
// side's value; changed on both to different values, it is a clash, which keeps this side's value and all three. An element
// added on one side is added; taken away on one side and unchanged on the other, it goes; taken away on one side and
// changed on the other, it stays, and a note says so. Order takes the other side's moves where this side made none. An
// item one side put somewhere else (a clip onto another track, a device into a container) isn't taken away and added: it
// goes where that side put it, with the other side's changes, and stays where this side put it if both moved it. A folder
// taken away goes without the tracks in it, lines of their own. So no item is in the merged song twice. What
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

// An ID a merge gave anew to what the other side added, as the other side has it and as this one does now: an item's, or a
// note's in a clip, by the clip's alias group or itself ("link 12", "clip 66"), as the merge's commit keeps it
// (VersionStore.h: VersionRecord::renumbered), so that a later merge of that line knows them again (docs/archive/format-audit.md, R9).
struct RenumberedID
{
    juce::String clip;
    std::uint64_t from = 0, to = 0;

    bool operator== (const RenumberedID&) const = default;
};

struct MergeResult
{
    juce::ValueTree song; // this side's, with the other's lines taken in
    std::vector<MergeClash> clashes;
    juce::StringArray notes; // what a merge can't decide for the user, such as "Bass: changed here, taken away there: kept"
    std::vector<RenumberedID> renumbered;
};

// A song's IDs as earlier merges gave them anew (each from, its item's and what names it, or its note's in that clip, made
// to): the other side's song, and where the lines parted, numbered as this side's before they are merged again. All at
// once, so that one ID given anew never meets another.
void translateIDs (juce::ValueTree song, const std::vector<RenumberedID>&);

// What the parting's song (base) and the other side's (theirs) are translated through before a merge of a line merged
// before (translateIDs()), from what this line's earlier merges of it recorded, oldest first (VersionHistory.h:
// findRenumbering()): the IDs of the items the parting holds, and of the notes in its clips, each the later merge's word over
// an earlier's; and, for any other ID of either song that one of those is translated to (an item the other line made after
// a merge gave its ID to one of theirs, on another computer whose count never went past it), a new one from makeID, so that
// no two items share an ID. Those given anew are in all too, and in givenAnew, which the merge's commit records with its own
// (MergeResult::renumbered), so that a later merge of that line translates them alike. What of the records isn't one to
// one (an ID 0, or one ID given to two: a damaged record, or one another history wrote) is left out (findLatestWords()). What
// the other side's song was translated through first (crossed: VersionHistory.h, crossRenumbering(), a note's by its clip as
// that numbers it) made the items under its targets this side's own, which count as no other ID either song holds.
struct Translation
{
    std::vector<RenumberedID> all, givenAnew;
};

Translation findTranslation (const juce::ValueTree& base, const juce::ValueTree& theirs, const std::vector<RenumberedID>& recorded,
                             const std::function<std::uint64_t()>& makeID, const std::vector<RenumberedID>& crossed = {});

// Records (oldest first) as findTranslation() takes them: the later record's word for each ID (an item's, a note's by its
// clip's group), without what isn't one to one (an ID 0, or one ID given to two).
std::vector<RenumberedID> findLatestWords (const std::vector<RenumberedID>& recorded);

// Whether IDs given anew are one to one, as a merge gives them: none 0, and within each group (the items', each clip's
// notes) no two with the same from, nor two with the same to. What isn't came from a damaged or a hand-made record.
bool areOneToOne (const std::vector<RenumberedID>&);

// The other side's plug-ins' knobs (SongPieces.h), by their devices' IDs as translateIDs() numbers them: all at once.
PluginParameterValues translateKnobs (PluginParameterValues, const std::vector<RenumberedID>&);

// Merges every line the other side changed since the two parted into a copy of this side's song: never some of them, as a
// merge is recorded whole. The lines are findChanges (base, theirs), as the merge's list holds them already (so that Merge
// doesn't compare two whole songs again), or worked out here. New IDs come from makeID (the song's: Song::makeID()).
MergeResult mergeSongs (const juce::ValueTree& base, const juce::ValueTree& ours, const juce::ValueTree& theirs,
                        const std::vector<SongChange>& lines, const std::function<std::uint64_t()>& makeID);
MergeResult mergeSongs (const juce::ValueTree& base, const juce::ValueTree& ours, const juce::ValueTree& theirs,
                        const std::function<std::uint64_t()>& makeID);

// The element a clash's path names in a song, invalid if it has none.
juce::ValueTree findByPath (const juce::ValueTree& song, const juce::StringArray& path);
} // namespace phazite
