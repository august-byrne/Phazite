// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's chord lane as a value (roadmap step S5.20, the chord lane's area): its chords and its key changes, each kind in
// the song's order, in Phazite's own types, and the conversion between the value and the CHORDLANE element with its CHORD
// and CHORDKEY children (Schema.h), the one place that reads and writes those by name. The song doesn't hold this value yet:
// its lane is still its tree's element, which the chord lane's code (model/chords/internal/Chords.h: getChordLaneValue())
// reads into it, so that what reads the song's chords and keys reads them as a value, whatever stores them.
//
// Whether the song has a chord lane at all is no part of the value: a song without one takes its keys from the key set last
// (Schema.h: LASTKEY), and the element stays in the song's tree as that sign (Chords.h: hasChordLane()).
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
    immer::flex_vector<ChordEntry> chords;
    immer::flex_vector<KeyEntry> keys;

    // The same value without a look inside: both kinds shared (ValueArea.h). And the same entries, compared.
    bool isSameAs (const ChordLaneValue&) const noexcept;
    bool operator== (const ChordLaneValue&) const;
};

// The song's chord lane read from its CHORDLANE element (an invalid tree reads as an empty lane): each kind's children in the
// element's order, each property as the chord lane's code has always read it: a start or a length that is absent reads as 0,
// one below 0 as 0; a degree that is absent as 1, kept within 1 to 7; an accidental absent as 0, kept within -1 to 1; an
// inversion absent as 0 (the chord lane's code keeps it within its type's notes, which this knows nothing of); a key's root
// kept within 0 to 11, or none where it has none. Nothing else is kept within a range here, which the loader does (model/
// project/internal/Project.h, makeConsistent()). A child of another type, and a property Phazite doesn't know, are no part of it.
ChordLaneValue readChordLaneValue (const juce::ValueTree& chordLaneElement);

// The song's chord lane appended to a CHORDLANE element (a copy of the song's, emptied, outside any song: WholeState.h): its
// CHORD elements first, then its CHORDKEY elements, each kind in its order, each written as the chord lane's code writes one:
// a chord's start, length, degree and type always, its accidental and its inversion only where they aren't 0; a key change's
// start, root and scale (a key without a root without one).
void writeChordLane (const ChordLaneValue&, juce::ValueTree& chordLaneCopy);
} // namespace phazite
