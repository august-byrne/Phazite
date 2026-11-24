// SPDX-License-Identifier: AGPL-3.0-or-later
// The MIDI links' area of the song's values (roadmap step S5.20; SongValues.h, whose recipe this follows): its parts, each of
// which a list of SongValues.h's or WholeState.h's asks in turn, a line for each area. Its value and the conversion of its
// elements are MidiLinksValue.h's. It holds no IDs of its own (a link names a device's: IDsHeld.h has nothing of it). Message
// thread, but for whole state's parts, which read only the values they are given.
//
// The links' MIDILINKS element stays in the song's tree, empty, where the song has one: added with the first link, as it
// always was, and the place a whole copy writes the links in. Its presence is written into the song's file and compared with
// the saved copy (an empty MIDILINKS is a difference from none), so it keeps its rules: who makes and takes away the element
// is the MIDI links' code (model/midi/internal/MidiLinks.h: the writer of a link, in that step, as it was made with the first
// link; and a version's links made the song's, makeMidiLinksAs(), which adds or takes it away as the other song has it),
// never a reader; the element's own coming and going is the tree's, which its listeners hear as before.
#pragma once

#include "model/base/internal/MidiLinksValue.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"

#include <juce_data_structures/juce_data_structures.h>

namespace phazite
{
class Song;

// What the values' listeners are told of the MIDI links (Listener::midiLinksChanged()): once for each change of its value. It
// listens to the links' area from its making to its end.
struct SongValues::MidiLinksTeller final : ValueArea<MidiLinksValue>::Listener
{
    MidiLinksTeller (ValueArea<MidiLinksValue>&, juce::ListenerList<SongValues::Listener>&);
    ~MidiLinksTeller() override;

    ValueArea<MidiLinksValue>& area;
    juce::ListenerList<SongValues::Listener>& listeners;

    void valueAreaChanged (const MidiLinksValue& before, const MidiLinksValue& after) override;

    JUCE_DECLARE_NON_COPYABLE (MidiLinksTeller)
};

// The MIDI links' part of the keeper (SongValues.h), its safety net: a MIDILINKS that comes into the song with links in it has
// them read in as the song's links (readHeldMidiLinks()), and is emptied as the adoption empties the song's own (what
// Phazite doesn't know of it goes too); a MIDILINK added to the song's MIDILINKS by itself is read into the links
// (withLinkTaken()) and taken out again while the song is written outside the history, and refused in a step. Nothing when
// one of its elements leaves the tree. And the links' adoption from the loaded tree, which the song's holdValues() asks.
struct SongValues::MidiLinksKeeper
{
    MidiLinksKeeper (SongValues&, Song&);

    // A child added to an element of the song's tree (the keeper's callback, but while the history undoes or redoes, for
    // every element, whoever's it is): nothing, at once, but for the song's MIDILINKS or a child added to it.
    void childAdded (juce::ValueTree& parent, juce::ValueTree& child);

    // The song's MIDILINKS emptied, once its reader read the links the loader made consistent into the area
    // (readHeldMidiLinksIn(), SongValues.cpp: adopt()). A song without a MIDILINKS keeps none, its links none.
    static void adopt (SongValues& into, Song& loaded);

    SongValues& values;
    Song& song;
    juce::ValueTree state;

    JUCE_DECLARE_NON_COPYABLE (MidiLinksKeeper)
};

// The MIDI links' part of whole state (WholeState.cpp: writeIn()): whether a child of an element in a copy of the song is the
// song's MIDILINKS (the child of its PHAZITE element); and a copy's MIDILINKS given the links kept (MidiLinksValue.h:
// writeMidiLinks(), in their order), where some are kept (a song that keeps everything in its tree keeps none: its MIDILINKS
// holds its links). Nothing for an element that isn't a MIDILINKS.
bool leadsToMidiLinks (const juce::ValueTree& parent, const juce::ValueTree& child);
void writeMidiLinksIn (juce::ValueTree& element, const SongState&);

// The MIDI links' reader of a song's tree (SongState.h: readSongTree(), a line of it): its MIDILINKS into the state, as the
// song holds them (none where it has none); no tree changed. Any thread.
void readHeldMidiLinksIn (const juce::ValueTree& song, SongState&);
} // namespace phazite
