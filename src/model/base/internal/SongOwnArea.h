// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's own part of the song's values (roadmap step S5.20; SongValues.h, whose recipe this follows), as far as it is
// built: what the song holds of its own (SongOwnValue.h: its root's properties and its LASTKEY, PIANOROLL and MERGING, in
// three values, SongOwnValue, SongStanding and SongOutside) is still its tree's, written by the setters (SongOwnValue.h:
// setSongOwn(), setSongStanding(), storeStandingOutsideHistory(), setSongOutside()) by what differs. Here: what the values'
// listeners are told of them (SongOwnChange: SongValues::Listener::songOwnChanged(), songStandingChanged(),
// songOutsideChanged()); the song's own part of the keeper, which tells them what the tree tells of them until the switch;
// and what of the tree's notifications is the song's own (isHeardAsSongOwn()), which every listener of the song's root
// leaves to the values' listener. The root's nextID (a count) and what Phazite doesn't know there stay the tree's, heard as
// before. Message thread.
#pragma once

#include "model/base/Song.h"
#include "model/base/internal/SongOwnValue.h"
#include "model/base/internal/SongValues.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>

namespace phazite
{
// What changed of the song's own value (SongOwnValue.h: SongOwnValue), as bits of parts, each its own: what listens takes each
// its own way (the views' watcher the notes alone, the versions a merge waiting). Set its members by name.
struct SongOwnChange
{
    enum Part : std::uint8_t
    {
        outline = 1 << 0,     // the outline applied last
        notes = 1 << 1,       // the project's notes
        idNumbering = 1 << 2, // the numbering the IDs are in
        patterns = 1 << 3,    // whether a tempo change leaves the patterns at their speed
        merging = 1 << 4      // a merge waiting: taken, a clash chosen, finished or cancelled
    };

    std::uint8_t parts = 0;

    bool has (std::uint8_t part) const noexcept { return (parts & part) != 0; }
};

// The song's own part of the keeper (SongValues.h), while the song's tree holds the song's own: it keeps the three values as
// the listeners were last told them (told: read at the start, as the song holds them, which no one is told) and tells what
// differs between those and what the song holds now, once for each change of a value, as the values' tellers will from the
// switch on:
//   - a setter's writes (SongOwnValue.h) are bracketed (SongValues::SongOwnWrite): what the tree tells meanwhile is marked,
//     and told as the bracket ends, once for the set, however many properties and elements it wrote (a loop's range, a
//     grid's five, a merge's record);
//   - what an Undo or a Redo of the song's history puts back is marked as the tree tells it, and told once the move is whole
//     (Song.h: HistoryListener, which the undo code tells after each of JUCE's undo() and redo() and each step taken back
//     alone), once for the move, whatever the step wrote; while it tells, isTellingHistoryMove() says so, for what follows
//     an Undo apart from an edit (VersionHistory.h: edited()), as the values' tellers will tell inside the Undo from the
//     switch on;
//   - anything else the tree tells of them (a write round the setters, which nothing of Phazite's makes once the song holds
//     its values) is told at once, a call for each of the tree's notifications;
//   - what was marked and is still untold (an Undo moved by hand, round the undo code) is told with the next change.
// Each value's listener function only where that value differs: songOwnChanged() with the parts that differ,
// songStandingChanged() with whether the key set last and the versions' names differ, songOutsideChanged() with whether what
// plays differs (the loop's four, the metronome), in that order. Nothing of the song's values is read (the getters read the
// song's tree, which holds the song's own until the switch).
struct SongValues::SongOwnKeeper final : private Song::HistoryListener
{
    SongOwnKeeper (SongValues&, Song&);
    ~SongOwnKeeper() override;

    // The keeper's callbacks, each for every element, whoever's it is, in an undo and a redo too: nothing, at once, but for
    // what is the song's own (isHeardAsSongOwn()).
    void propertyChanged (const juce::ValueTree& tree, const juce::Identifier& property);
    void childChanged (const juce::ValueTree& parent, const juce::ValueTree& child); // added or taken out

    // A setter's writes, from their start to their end; brackets nest.
    void beginWrite() noexcept { ++writing; }
    void endWrite();

    SongValues& values;
    Song& song;
    SongOwnValue toldOwn {};
    SongStanding toldStanding {};
    SongOutside toldOutside {};
    int writing = 0;          // setters' brackets open
    bool marked = false;      // what the tree told, not told yet
    bool tellingMove = false; // telling what a move of the history put back

    void heard();
    void tell();
    void historyMoved() override;

    JUCE_DECLARE_NON_COPYABLE (SongOwnKeeper)
};

// Whether what the song's tree tells is the song's own, which a listener of the song's root hears from the values' listener
// alone (SongValues::Listener::songOwnChanged(), songStandingChanged(), songOutsideChanged()) and leaves alone in its tree
// callbacks, while the tree is still its storage. Of an element (children in another order): the song's LASTKEY, PIANOROLL or
// MERGING, or one in it. Of a property changed: one of the root's own that the three values hold (SongOwnValue.h:
// isSongOwnProperty(): not nextID, a count, nor a property Phazite doesn't know, which the tree tells as before), or any
// property of the song's LASTKEY, PIANOROLL or MERGING. Of a child added or taken out: the song's LASTKEY, PIANOROLL or
// MERGING, or a child of one. In SongOwnArea.cpp.
bool isHeardAsSongOwn (const juce::ValueTree& element);
bool isHeardAsSongOwn (const juce::ValueTree& element, const juce::Identifier& property);
bool isHeardAsSongOwn (const juce::ValueTree& parent, const juce::ValueTree& child);
} // namespace phazite
