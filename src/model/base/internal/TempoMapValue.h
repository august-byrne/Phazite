// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's tempo map as a value (roadmap step S5.20, the tempo map's area): its tempos and time signatures, each kind by beat,
// in Phazite's own types, and the conversion between the value and the TEMPOMAP element with its TEMPO and TIMESIGNATURE
// children (Schema.h), the one place that reads and writes those by name. The song doesn't hold this value yet: its tempo map
// is still its tree's element, which the tempo map's code (model/transport/internal/Tempo.h: getTempoMapValue()) reads into
// it, so that what reads the tempo map reads it as a value, whatever stores it.
//
// Its own header, beside SongValues.h (one header for each area's values): the model's base includes no other area, so these
// are its own types; the tempo map's code converts them to TempoChange and TimeSignatureChange (model/transport/
// TempoValues.h), which stay its API's. immer is the model's alone (tools/check-architecture.sh: immer-outside-model).
#pragma once

#include "model/base/Beats.h"
#include "model/base/internal/Immer.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

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
// as the tempo map's code has always read it (a beat that is absent reads as 0, a bpm as 120, a curve as 1, kept within -1 and
// 1, and a numerator and a denominator as 4; nothing else is kept within a range here, which the loader does:
// model/project/internal/Project.h, makeConsistent()), each kind sorted stably by beat, so that two at one beat keep the
// element's order; a kind the element has none of holds one from the song's start, as makeDefaultTempoMap() gives it. A
// property or a child Phazite doesn't know is no part of it.
TempoMapValue readTempoMapValue (const juce::ValueTree& tempoMapElement);

// One kind of it alone, read as readTempoMapValue() reads it: for what asks for one kind of the song's tempo map while its
// tree holds it, so that it reads no more than it did (model/transport/Tempo.cpp).
immer::flex_vector<TempoEntry> readTempoEntries (const juce::ValueTree& tempoMapElement);
immer::flex_vector<SignatureEntry> readSignatureEntries (const juce::ValueTree& tempoMapElement);

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
} // namespace phazite
