// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's MIDI links as a value (roadmap step S5.20, the MIDI links' area): each link of a hardware controller to a
// parameter, in the song's order, in Phazite's own types, and the conversion between the value and the MIDILINKS element with
// its MIDILINK children (Schema.h), the one place that reads and writes those by name. The song doesn't hold this value yet:
// its links are still its tree's elements, which the MIDI links' code (model/midi/internal/MidiLinks.h: getMidiLinksValue())
// reads into it, so that what reads the song's links reads them as a value, whatever stores them.
//
// Its own header, beside SongValues.h and the other areas' (one header for each area's values; the name MidiLinkValues.h is
// model/midi's already, for the links' API): the model's base includes no other area, so these are its own types, a
// parameter by its device's ID and its text as the element holds them; the MIDI links' code makes a ParameterRef of them
// (MidiLinks.h: readMidiLinkParameter()). immer is the model's alone (tools/check-architecture.sh: immer-outside-model).
#pragma once

#include "model/base/internal/Immer.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>

namespace phazite
{
// A link as the song holds it (Schema.h: MIDILINK): the controller, by its channel and its number, and the parameter it
// turns, by its device's ID (any item of the song's that has parameters: a device, a chain, a send, a fader, a modulator, a
// macro) and its parameter's text (a macro's by the word schema::midiLinkMacroValue).
struct MidiLinkEntry
{
    int channel = 1, number = 0;
    std::uint64_t device = 0;
    juce::String parameter;

    bool operator== (const MidiLinkEntry&) const noexcept;
};

// The song's links, in the song's order (the element's: of two links of one controller or one parameter, the first stays,
// model/midi/internal/MidiLinks.h: keepOneMidiLinkEach()). Copies are cheap and share what they hold.
struct MidiLinksValue
{
    immer::flex_vector<MidiLinkEntry> links;

    // The same value without a look inside: its links shared (ValueArea.h). And the same links, compared.
    bool isSameAs (const MidiLinksValue& other) const noexcept { return links.identity() == other.links.identity(); }
    bool operator== (const MidiLinksValue& other) const { return links == other.links; }
};

// The song's links read from its MIDILINKS element (an invalid tree reads as none): each MIDILINK child in the element's
// order, each property as the MIDI links' code has always read it (a channel or a number that is absent reads as 0, a device's
// ID as readID() reads one, text or a number; nothing kept within a range here, which the loader does: model/project/internal/
// Project.h, makeConsistent()). A child of another type is no link, and a property Phazite doesn't know no part of one.
MidiLinksValue readMidiLinksValue (const juce::ValueTree& midiLinksElement);

// One link read so from its MIDILINK element: for what reads a link's element before the song is made of it (the loader:
// model/project/internal/Project.h, makeConsistent()).
MidiLinkEntry readMidiLinkEntry (const juce::ValueTree& midiLinkElement);

// The song's links appended to a MIDILINKS element (a copy of the song's, emptied, outside any song: WholeState.h), each a
// MIDILINK as the MIDI links' code writes one: its channel, its number, its device's ID as text ("1042") and its parameter.
void writeMidiLinks (const MidiLinksValue&, juce::ValueTree& midiLinksCopy);
} // namespace phazite
