// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's own part of the song's values (SongValues.h, whose recipe this follows): what the song holds
// of its own (SongOwnValue.h: its root's properties and its LASTKEY, PIANOROLL and MERGING) in three areas (SongValues.h:
// songOwn, a plain ValueArea<SongOwnValue>; songStanding, a keyed ValueArea<SongStanding>; songOutside, an
// OutsideValue<SongOutside>), which its setters set (SongOwnValue.h: setSongOwn(), setSongStanding(),
// storeStandingOutsideHistory(), setSongOutside(), through SongValues' setOwn(), setStanding(), setStandingOutsideHistory(),
// setOutside(), defined here). Here: what the values' listeners are told of them (SongOwnChange: SongValues::Listener::
// songOwnChanged(), songStandingChanged(), songOutsideChanged()), by each area's teller; and its part of whole state and its
// reader of a song's tree. Message thread, but for whole state's part and the reader, which read only what they are given.
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

// The song's own part of whole state (WholeState.h; the recipe's step 5): into a whole copy's root (PHAZITE), the kept
// values' song's own (WholeState.h: SongState: songOwn, songStanding, songOutside), its properties and its LASTKEY, PIANOROLL
// and MERGING, as SongOwnValue.h's writeSongOwnInto() writes them (writeSongTree() then puts the root's children in the
// description's order: SongState.h), and its count of IDs (nextID) where it is kept; nothing
// for another element. And whether two songs' roots, each with its kept values, hold the same of what counts of the song's
// own: its own value (the outline, the notes, the numbering, patterns' speed, a merge waiting) and its key set last (LASTKEY,
// counted; the versions' names and how the song plays and shows count nothing, nor its count of IDs). Both of Project's
// comparisons with the saved copy compare the song's own so (the quick one, isSongOwnAsSaved(), and the whole song's,
// WholeState.h's holdSameSong()), so that the two agree. Any thread.
struct SongState;
void writeSongOwnIn (juce::ValueTree& element, const SongState&);

// The song's own reader of a song's tree (SongState.h: readSongTree(), its first line): its root's properties and its LASTKEY,
// PIANOROLL and MERGING into the state's three values, and its count of IDs, as the song holds them; no tree changed. Any
// thread.
void readHeldSongOwnIn (const juce::ValueTree& song, SongState&);
bool holdSameSongOwn (const SongState&, const SongState&);
} // namespace phazite
