// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's chord lane as a value (roadmap step S5.20, the chord lane's area): its chords and its key changes, each kind in
// the song's order, in Phazite's own types, and the conversion between the value and the CHORDLANE element with its CHORD
// and CHORDKEY children (Schema.h), the one place that reads and writes those by name. The song holds its lane as this value
// in an area of its values (SongValues.h, ChordLaneArea.h), which the chord lane's code (model/chords/internal/Chords.h)
// computes each edit on; its tree's CHORDLANE holds none of its chords and keys, the place a whole copy writes them in.
//
// Whether the song has a chord lane at all is a part of the value (placed): a song without one takes its keys from the key set
// last (Schema.h: LASTKEY), and a whole copy writes an empty CHORDLANE for a lane placed without chords or keys. The chord
// lane's writers set it in the step that adds or takes away the song's CHORDLANE, which stays in its tree beside it, equal to
// it (tests/SongCheck.cpp), until the tree goes (roadmap step S5.20's end); Chords.h's hasChordLane() reads it. A song without
// one holds an empty value, not placed.
//
// Its own header, beside SongValues.h and the other areas' (one header for each area's values): the model's base includes no
// other area, and knows no chord types nor scales, so a chord's type and a key's scale are their saved words here, which the
// chord lane's code converts (model/chords/ChordValues.h). immer is the model's alone (tools/check-architecture.sh:
// immer-outside-model).
#pragma once

#include "model/base/Beats.h"
#include "model/base/internal/Immer.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

namespace phazite
{
// A chord as the lane holds it (Schema.h: CHORD): where it starts and how long it lasts, in beats; its numeral, by its degree
// in the key (1 to 7), its accidental (-1 flat, 0, 1 sharp), its type by its saved word ("maj", "min7"...) and its
// inversion (from 0).
struct ChordEntry
{
    BeatPosition start;
    BeatDuration length = BeatDuration::fromBeats (4.0);
    int degree = 1, accidental = 0, inversion = 0;
    juce::String type;

    bool operator== (const ChordEntry&) const noexcept;
};

// A key change as the lane holds it (Schema.h: CHORDKEY): from where it starts, the key's root (0 to 11, C to B), or none
// (-1: an element without one, which is no key), and its scale by its saved word.
struct KeyEntry
{
    BeatPosition start;
    int root = -1;
    juce::String scale;

    bool operator== (const KeyEntry&) const noexcept;
};

// The song's chord lane: its chords and its key changes, each kind in the element's order (the readers sort them by their
// starts, two at one start in that order). Copies are cheap and share what they hold.
struct ChordLaneValue
{
    immer::flex_vector<ChordEntry> chords {};
    immer::flex_vector<KeyEntry> keys {};
    bool placed = false; // whether the song has a lane (above): one with chords or keys has one

    // The same value without a look inside: both kinds shared, and placed alike (ValueArea.h). And the same entries, compared,
    // and placed alike.
    bool isSameAs (const ChordLaneValue&) const noexcept;
    bool operator== (const ChordLaneValue&) const;
};

// The song's chord lane read from its CHORDLANE element (an invalid tree reads as an empty lane, not placed; a valid one is
// placed): each kind's children in the
// element's order, each property as the chord lane's code has always read it, a number's text as the song reads one
// (SongText.h: readValueNumber()): a start or a length that is absent or no finite number reads as 0, one below 0 as 0; a
// degree that is absent as 1, kept within 1 to 7; an accidental absent as 0, kept within -1 to 1; an inversion absent as 0
// (the chord lane's code keeps it within its type's notes, which this knows nothing of); a key's root kept within 0 to 11,
// or none where it has none. Nothing else is kept within a range here, which the loader does (model/project/internal/
// Project.h, makeConsistent()). A child of another type, and a property Phazite doesn't know, are no part of it. What the
// tests read a lane by.
ChordLaneValue readChordLaneValue (const juce::ValueTree& chordLaneElement);

// The song's chord lane read from a CHORDLANE as the song holds one (a song made of a file's state, SongState.h: readSongTree();
// a version's lane and a merge's choice, ChordLaneArea.h; placed as readChordLaneValue() places it): each kind's children in the
// element's order, each property as its description reads it (SongText.h: readDescribed()): a start that is absent or no finite
// number as 0, a length as 4, a degree as 1, an accidental and an inversion as 0, each within its range and made whole where it
// is a whole one; a key's root within 0 to 11 where it is a finite number, else none (as the loader leaves it); a chord's type
// and a key's scale as their words are written. A child of another type, and a property Phazite doesn't know, are no part of it.
// Its chords kept apart (keepChordsApart(), below): a file from others may hold chords that overlap, or one without a length (4
// beats long, its description's fallback) before another.
ChordLaneValue readHeldChordLane (const juce::ValueTree& chordLaneElement);

// How near two beats of the lane are that count as one (a millionth of a beat): a chord that reaches past the next one's
// start by no more than this doesn't overlap it, and one no longer than this has no length. The chord lane's code makes
// room for a chord it puts by the same nearness (model/chords/Chords.cpp: clearChords()).
constexpr double chordLaneNear = 1.0e-6;

// The lane with its chords apart, as the chord lane's code always leaves them (it clears a chord's range before it puts it
// there: model/chords/Chords.cpp, putLaneChord(), clearChords()): each chord with a length of its own, by their starts (two at
// one start in the lane's order), reaching no further than the next one's start, and gone where nothing of it would then be
// left (of two at one start, the later in the lane's order stays, as a chord put later replaces one there); a chord without
// a length of its own (none longer than chordLaneNear) stays as it is, and is no chord's next. The chords that stay in the
// lane's order, the keys and placed as they are; the same value where nothing overlaps. What everything that enters the song's
// lane is read with keeps (readHeldChordLane(), withEntryTaken()).
ChordLaneValue keepChordsApart (const ChordLaneValue&);

// Whether a lane's chords are apart, as keepChordsApart() keeps them (it would change nothing): what the song's consistency
// check asks of its value.
bool holdsChordsApart (const ChordLaneValue&);

// The song's chord lane appended to a CHORDLANE element (a copy of the song's, emptied, outside any song: WholeState.h): its
// CHORD elements first, then its CHORDKEY elements, each kind in its order, each written as the chord lane's code writes one:
// a chord's start, length and degree always, its type where it has one, its accidental and its inversion only where they
// aren't 0; a key change's start, its root where it has one and its scale where it has one (the chord lane's code writes
// both, always: only a file from others holds a key without them).
void writeChordLane (const ChordLaneValue&, juce::ValueTree& chordLaneCopy);
} // namespace phazite
