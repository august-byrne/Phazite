// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's MIDI links as a value (roadmap step S5.20, the MIDI links' area): each link of a hardware controller to a
// parameter, in the song's order, in Phazite's own types, and the conversion between the value and the MIDILINKS element with
// its MIDILINK children (Schema.h), the one place that reads and writes those by name. The song holds this value in an area of
// its values (SongValues.h, MidiLinksArea.h), which the MIDI links' code (model/midi/internal/MidiLinks.h) computes each edit
// on; a whole copy writes the links in a MIDILINKS while there is one (WholeState.h).
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
#include <optional>

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
// keepOneLinkEach()). Copies are cheap and share what they hold.
struct MidiLinksValue
{
    immer::flex_vector<MidiLinkEntry> links;

    // The same value without a look inside: its links shared (ValueArea.h). And the same links, compared.
    bool isSameAs (const MidiLinksValue& other) const noexcept { return links.identity() == other.links.identity(); }
    bool operator== (const MidiLinksValue& other) const { return links == other.links; }
};

// The song's links read from its MIDILINKS element (an invalid tree reads as none): each MIDILINK child in the element's
// order, each property as the MIDI links' code has always read it, a number's text as the song reads one (SongText.h:
// readValueNumber()): a channel or a number that is absent, or no finite number, reads as 0, one that isn't whole truncated to
// a whole one; a device's ID as readID() reads one, text or a number; nothing kept within a range here, which the loader does:
// model/project/internal/Project.h, makeConsistent()). A child of another type is no link, and a property Phazite doesn't
// know no part of one. What the loader's keepOneMidiLinkEach()
// reads (MidiLinks.h), and the tests.
MidiLinksValue readMidiLinksValue (const juce::ValueTree& midiLinksElement);

// One link read so from its MIDILINK element: for what reads a link's element before the song is made of it (the loader:
// model/project/internal/Project.h, makeConsistent()).
MidiLinkEntry readMidiLinkEntry (const juce::ValueTree& midiLinkElement);

// The song's links read from a MIDILINKS as the song holds them (a song made of a file's state, SongState.h:
// readSongTree(); a version's links and a merge's choice, MidiLinksArea.h): each MIDILINK child in the element's
// order, each number as its description reads it (SongText.h: readValueNumber()), made whole: a link whose channel or number
// is absent, no finite number, or outside its range (Schema.h: goingOutside(), which brought within it would be another
// controller) goes, as the loader takes it away; so does one that names no parameter (no device's ID, or an empty text). Of
// two links of one controller or of one parameter, the first stays (keepOneLinkEach()). A child of another type, and a
// property Phazite doesn't know, are no part of it.
MidiLinksValue readHeldMidiLinks (const juce::ValueTree& midiLinksElement);

// One link read so from its MIDILINK element (readHeldMidiLinks()): none where it is no link the song can hold.
std::optional<MidiLinkEntry> readHeldMidiLink (const juce::ValueTree& midiLinkElement);

// One link of each controller (its channel and its number) and one of each parameter (its device's ID and its text, as the
// link holds them): of two, the first in the value's order stays, the other goes. The one rule for both the song's value
// (what enters it: readHeldMidiLinks(); and the writers that could leave two: model/midi/MidiLinks.cpp,
// forgetMidiLinksOfTheGone()) and the loader's elements (MidiLinks.h: keepOneMidiLinkEach()). The same value where each is
// one already.
MidiLinksValue keepOneLinkEach (const MidiLinksValue&);

// Whether the links hold one of each controller and one of each parameter, as keepOneLinkEach() keeps them (it would change
// nothing): what the song's consistency check asks of its value.
bool holdsOneLinkEach (const MidiLinksValue&);

// A link as a MIDILINK element, as the MIDI links' code writes one: its channel, its number, its device's ID as text ("1042")
// and its parameter. And the song's links appended to a MIDILINKS element (a copy of the song's, emptied, outside any song:
// WholeState.h), each so, in their order.
juce::ValueTree writeMidiLinkElement (const MidiLinkEntry&);
void writeMidiLinks (const MidiLinksValue&, juce::ValueTree& midiLinksCopy);
} // namespace phazite
