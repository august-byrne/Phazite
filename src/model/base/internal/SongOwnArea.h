// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's own part of the song's values (roadmap step S5.20; SongValues.h, whose recipe this follows): what the song holds
// of its own (SongOwnValue.h: its root's properties and its LASTKEY, PIANOROLL and MERGING) in three areas (SongValues.h:
// songOwn, a plain ValueArea<SongOwnValue>; songStanding, a keyed ValueArea<SongStanding>; songOutside, an
// OutsideValue<SongOutside>), which its setters set (SongOwnValue.h: setSongOwn(), setSongStanding(),
// storeStandingOutsideHistory(), setSongOutside(), through SongValues' setOwn(), setStanding(), setStandingOutsideHistory(),
// setOutside(), defined here). Here: what the values' listeners are told of them (SongOwnChange: SongValues::Listener::
// songOwnChanged(), songStandingChanged(), songOutsideChanged()), by each area's teller; the song's own part of the keeper:
// its adoption as the song holds its values, and its safety net; and what of the tree's notifications is the song's own
// (isHeardAsSongOwn()), which every listener of the song's root leaves to the values' listener. Message thread.
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

// Each area's teller (SongValues.h's recipe, step 2): a listener of its area that tells the values' listener of each change of
// the area's value (a step's set, an Undo's or a Redo's move, a write outside the history), by what differs between the value
// before and after, once for the change, only where something does: songOwnChanged() with the parts that differ,
// songStandingChanged() with whether the key set last and the versions' names differ, songOutsideChanged() with whether what
// plays differs (the loop's four, the metronome). Each reads nothing but the two values it is handed.
struct SongValues::SongOwnTeller final : private ValueArea<SongOwnValue>::Listener
{
    SongOwnTeller (ValueArea<SongOwnValue>&, juce::ListenerList<SongValues::Listener>&);
    ~SongOwnTeller() override;

    void valueAreaChanged (const SongOwnValue& before, const SongOwnValue& after) override;

    ValueArea<SongOwnValue>& area;
    juce::ListenerList<SongValues::Listener>& listeners;

    JUCE_DECLARE_NON_COPYABLE (SongOwnTeller)
};

struct SongValues::SongStandingTeller final : private ValueArea<SongStanding>::Listener
{
    SongStandingTeller (ValueArea<SongStanding>&, juce::ListenerList<SongValues::Listener>&);
    ~SongStandingTeller() override;

    void valueAreaChanged (const SongStanding& before, const SongStanding& after) override;

    ValueArea<SongStanding>& area;
    juce::ListenerList<SongValues::Listener>& listeners;

    JUCE_DECLARE_NON_COPYABLE (SongStandingTeller)
};

struct SongValues::SongOutsideTeller final : private OutsideValue<SongOutside>::Listener
{
    SongOutsideTeller (OutsideValue<SongOutside>&, juce::ListenerList<SongValues::Listener>&);
    ~SongOutsideTeller() override;

    void outsideValueChanged (const SongOutside& before, const SongOutside& after) override;

    OutsideValue<SongOutside>& area;
    juce::ListenerList<SongValues::Listener>& listeners;

    JUCE_DECLARE_NON_COPYABLE (SongOutsideTeller)
};

// The song's own part of the keeper (SongValues.h):
//   - its adoption (adopt(), which SongValues::adopt() asks first, as the song holds its values): what the loaded tree's root
//     holds of the song's own, read as the song holds it (SongOwnValue.h: readHeldSongOwn(), readHeldSongStanding(),
//     readHeldSongOutside(); the loader made it so: keepSongOwnAsHeld()) by its reader (readHeldSongOwnIn()), set into the three
//     areas outside the history, and its count of IDs (Schema.h: nextID) onto the song (Song.h: setIDCount()), as every area's
//     (SongValues.cpp: adopt()); then (adopt()) the root's every property and its LASTKEY,
//     PIANOROLL and MERGING taken out of the tree, and with them what Phazite doesn't know there (a property of the root's, an
//     element of a type it doesn't know, with everything in it: dropped as the song opens, the one place that drops them, as
//     the areas' adoptions drop what Phazite doesn't know in their elements);
//   - its safety net (the keeper's callbacks, nothing while the history undoes or redoes a step): a property of the root's own
//     (SongOwnValue.h: isSongOwnProperty()) or its count (nextID) written to the song's root, or a LASTKEY, PIANOROLL or
//     MERGING added to it: in a step, refused (a JUCE assertion, as a lone CLIP is: the step that wrote it can't take it out
//     again, as its Undo would find it gone; it stays where it was written until that Undo: JUCE workaround W62
//     (docs/juce-workarounds.md)); outside the history (Song::OutsideHistory), read in as the song holds it (the property's or
//     the element's value in place of the area's own, through the setters; the count as it is) and taken out, counted
//     (countSongOwnAdopted()). No path of Phazite's writes either to the tree of a song that holds its values.
struct SongValues::SongOwnKeeper final
{
    SongOwnKeeper (SongValues&, Song&);

    void propertyChanged (const juce::ValueTree& tree, const juce::Identifier& property);
    void childAdded (juce::ValueTree& parent, juce::ValueTree& child);

    static void adopt (SongValues& into, Song& loaded);

    SongValues& values;
    Song& song;

    JUCE_DECLARE_NON_COPYABLE (SongOwnKeeper)
};

// Whether what the song's tree tells is the song's own, which a listener of the song's root hears from the values' listener
// alone (SongValues::Listener::songOwnChanged(), songStandingChanged(), songOutsideChanged()) and leaves alone in its tree
// callbacks: what its safety net takes in (SongOwnKeeper), the tree holding none of it. Of an element (children in another
// order): the song's LASTKEY, PIANOROLL or MERGING, or one in it. Of a property changed: one of the root's own that the three
// values hold (SongOwnValue.h: isSongOwnProperty()) or its count of IDs (nextID), or any property of the song's LASTKEY,
// PIANOROLL or MERGING. Of a child added or taken out: the song's LASTKEY, PIANOROLL or MERGING, or a child of one. In
// SongOwnArea.cpp.
bool isHeardAsSongOwn (const juce::ValueTree& element);
bool isHeardAsSongOwn (const juce::ValueTree& element, const juce::Identifier& property);
bool isHeardAsSongOwn (const juce::ValueTree& parent, const juce::ValueTree& child);

// The song's own part of whole state (WholeState.h; the recipe's step 5): into a whole copy's root (PHAZITE), the kept
// values' song's own (WholeState.h: SongState: songOwn, songStanding, songOutside), its properties and its LASTKEY, PIANOROLL
// and MERGING, as SongOwnValue.h's writeSongOwnInto() writes them (the elements after the root's other children: the
// placeholders, its track list and its master, in the tree's order), and its count of IDs (nextID) where it is kept; nothing
// for another element. And whether two songs' roots, each with its kept values, hold the same of what counts of the song's
// own: its own value (the outline, the notes, the numbering, patterns' speed, a merge waiting) and its key set last (LASTKEY,
// counted; the versions' names and how the song plays and shows count nothing, nor its count of IDs); true for another
// element. Both of Project's comparisons with the saved copy compare the song's own so (the quick one, isSongOwnAsSaved(), and
// the whole song's, through WholeState.h's holdSameKeptOf()), so that the two agree. Any thread.
struct SongState;
void writeSongOwnIn (juce::ValueTree& element, const SongState&);

// The song's own reader of a song's tree (SongState.h: readSongTree(), its first line): its root's properties and its LASTKEY,
// PIANOROLL and MERGING into the state's three values, and its count of IDs, as the song holds them; no tree changed. Any
// thread.
void readHeldSongOwnIn (const juce::ValueTree& song, SongState&);
bool holdSameSongOwn (const SongState&, const SongState&);
bool holdSameSongOwnOf (const juce::ValueTree& element, const SongState&, const juce::ValueTree& otherElement, const SongState&);
} // namespace phazite
