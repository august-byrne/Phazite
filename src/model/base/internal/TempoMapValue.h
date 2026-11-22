// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's tempo map as a value (roadmap step S5.20, the tempo map's area): its tempos and time signatures, each kind by beat,
// in Phazite's own types, and the conversion between the value and the TEMPOMAP element with its TEMPO and TIMESIGNATURE
// children (Schema.h), the one place that reads and writes those by name. The song holds its tempo map as this value, in an
// area of its values (SongValues.h), which the tempo map's code (model/transport/internal/Tempo.h) reads and computes each
// edit on; its tree holds the TEMPOMAP element alone, empty, the place a whole copy of the song writes the map in
// (WholeState.h).
//
// Its own header, beside SongValues.h (one header for each area's values): the model's base includes no other area, so these
// are its own types; the tempo map's code converts them to TempoChange and TimeSignatureChange (model/transport/
// TempoValues.h), which stay its API's. immer is the model's alone (tools/check-architecture.sh: immer-outside-model).
#pragma once

#include "model/base/Beats.h"
#include "model/base/internal/Immer.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <cstddef>
#include <optional>

namespace phazite
{
// A tempo as the song's tempo map holds it (Schema.h: TEMPO): from a beat on, in quarter notes a minute, and its line to the
// next: 1 a step (held, then a jump to the next: what a TEMPO without a curve holds), 0 a straight ramp, and between, the ramp
// bends (TempoValues.h: TempoChange::curve).
struct TempoEntry
{
    BeatPosition beat;
    double bpm = 120.0;
    float curve = 1.0f;

    // Written out, not defaulted: a defaulted one would compare the bpm and the curve with a floating-point ==, which GCC's
    // -Wfloat-equal refuses.
    bool operator== (const TempoEntry&) const noexcept;
};

// A time signature as the song's tempo map holds it (Schema.h: TIMESIGNATURE): from a beat on, so many beats (numerator) of
// a note value (denominator) in a bar.
struct SignatureEntry
{
    BeatPosition beat;
    int numerator = 4, denominator = 4;

    bool operator== (const SignatureEntry&) const noexcept;
};

// The song's tempo map: each kind by beat, two at one beat in the order they came (a stable order), at least one of each.
// Copies are cheap and share what they hold.
struct TempoMapValue
{
    immer::flex_vector<TempoEntry> tempos;
    immer::flex_vector<SignatureEntry> signatures;

    // The same value without a look inside: both kinds shared (ValueArea.h). And the same entries, compared.
    bool isSameAs (const TempoMapValue&) const noexcept;
    bool operator== (const TempoMapValue&) const;
};

// What a song's tempo map holds when it has nothing of its own: 120 BPM, a step, and 4/4, both from the song's start (as
// model/transport/internal/Tempo.h's ensureTempoMap() writes them).
TempoMapValue makeDefaultTempoMap();

// A tempo map's value, read from its TEMPOMAP element (an invalid tree reads as an element without children): each property
// as the tempo map's code has always read it, each number as the loader reads one (SongText.h: readValueNumber(), the same
// number for the same text wherever it is read; a beat that is absent or no finite number reads as 0, a bpm as 120, a curve
// as 1, kept within -1 and 1, and a numerator and a denominator as 4; nothing else is kept within a range here, which the
// loader does:
// model/project/internal/Project.h, makeConsistent()), each kind sorted stably by beat, so that two at one beat keep the
// element's order; a kind the element has none of holds one from the song's start, as makeDefaultTempoMap() gives it. A
// property or a child Phazite doesn't know is no part of it.
TempoMapValue readTempoMapValue (const juce::ValueTree& tempoMapElement);

// One kind of it alone, read as readTempoMapValue() reads it.
immer::flex_vector<TempoEntry> readTempoEntries (const juce::ValueTree& tempoMapElement);
immer::flex_vector<SignatureEntry> readSignatureEntries (const juce::ValueTree& tempoMapElement);

// A tempo map's value as the song takes one in (Song::holdValues(), of a file the loader made consistent; and the song's keeper,
// a safety net: SongValues.h): each number as its description reads it (Schema.h: an absent or unreadable bpm reads as 120, a
// numerator and a denominator as 4, a beat as 0, a curve as 1) and kept within its range (a whole number rounded, the
// denominator a power of two), each kind sorted stably by beat, so that two at one beat keep the element's order, and a kind
// the element has none of holding one from the song's start (makeDefaultTempoMap()'s). Nothing else is made to hold together:
// a first tempo or time signature not at the song's start (another tool's file) reads as the first, as readTempoMapValue()
// reads it. A property or a child Phazite doesn't know is no part of it.
TempoMapValue readHeldTempoMap (const juce::ValueTree& tempoMapElement);

// A tempo map's value with one more entry, read from its TEMPO or TIMESIGNATURE element as readHeldTempoMap() reads one, after
// those of its kind at its beat, as the element appended to the map would be read: the song's keeper's safety net
// (SongValues.h). The same value for an element of another type.
TempoMapValue withEntryTaken (const TempoMapValue&, const juce::ValueTree& entryElement);

// Whether a tempo map's value holds what the song holds of one: each kind not empty and in the order of its beats, and each
// number within its description's range, as readHeldTempoMap() keeps it (Schema.h). What a merge's choice may set into the
// song (model/versions/VersionsModel.cpp: chooseClash()), and what the song's consistency check asks of its value.
bool isHeldAsTheSongHoldsIt (const TempoMapValue&);

// An entry of a kind, at its place, moved to where its beat puts it among the others, as its element would be read with that
// beat (the entries in their file's order, sorted by beat, those at one beat staying as they were): moved later, before any
// already at its new beat; moved earlier, after them; at its own beat still, where it was. And the place it lands.
immer::flex_vector<TempoEntry> placeByBeat (const immer::flex_vector<TempoEntry>&, size_t index, const TempoEntry& moved, size_t& place);
immer::flex_vector<SignatureEntry> placeByBeat (const immer::flex_vector<SignatureEntry>&, size_t index, const SignatureEntry& moved,
                                                size_t& place);

// Appends a tempo map's value to a TEMPOMAP element (a copy of the song's, emptied, outside any song: WholeState.h): its TEMPO
// elements first, then its TIMESIGNATURE elements, each kind by beat, each written as the tempo map's code writes one: a
// tempo's beat and bpm always, its curve only where it isn't 1 (a step); a time signature's beat, numerator and denominator.
void writeTempoMap (const TempoMapValue&, juce::ValueTree& tempoMapCopy);

// A tempo map's value with a property of one of its entries given a value as the value would read it from an element that
// held it, or taken away (a void value: as if the element hadn't it): a merge's clash on a tempo's or a time signature's
// property, chosen. The entry is named by its step in the clash's path (model/versions/internal/SongMerge.h), its type and its
// key, the beat with six decimals ("TEMPO:16.000000", Schema.h: getElementKey()): the first of its kind with that key, as
// the path finds it in the element. A beat set moves the entry among its kind to its new beat: before any already there if it
// moved later, after them if it moved earlier, as the element would be read.
// Whether the step names an entry of the value's and the property is one of that entry's kind's (else nothing changes).
bool setTempoMapProperty (TempoMapValue&, const juce::String& step, const juce::Identifier& property, const juce::var& value);

// A tempo map's value with the entry a merge's clash names by its step (as setTempoMapProperty() finds it) made what an
// element holds: put in its place where the value holds one there (moved where its beat puts it, as placeByBeat() moves one,
// where it differs), else after those of its kind at its beat; taken out for an invalid element (the same value where the
// value holds none there). The element read as readHeldTempoMap() reads one, as everything that enters the song's tempo map
// is (whether the map then holds what the song holds of one, a kind not left empty, is the caller's to ask:
// isHeldAsTheSongHoldsIt()). None for a step that names neither kind, or an element of another type than the step's.
std::optional<TempoMapValue> withEntryAt (const TempoMapValue&, const juce::String& step, const juce::ValueTree& element);

// The same with the entry another value holds at that step (taken out where it holds none there); and whether a value holds an
// entry at a step.
std::optional<TempoMapValue> withEntryAt (const TempoMapValue&, const juce::String& step, const TempoMapValue& from);
bool holdsEntryAt (const TempoMapValue&, const juce::String& step);
} // namespace phazite
