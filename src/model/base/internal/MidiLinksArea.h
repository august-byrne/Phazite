// SPDX-License-Identifier: AGPL-3.0-or-later
// The MIDI links' area of the song's values (SongValues.h, whose recipe this follows): its parts, each of
// which a list of SongValues.h's or WholeState.h's asks in turn, a line for each area. Its value and the conversion of its
// elements are MidiLinksValue.h's. It holds no IDs of its own (a link names a device's: IDsHeld.h has nothing of it). Message
// thread, but for whole state's parts, which read only the values they are given.
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

// The MIDI links' part of whole state (WholeState.cpp: writeIn()): whether a child of an element in a copy of the song is the
// song's MIDILINKS (the child of its PHAZITE element); and a copy's MIDILINKS given the links kept (MidiLinksValue.h:
// writeMidiLinks(), in their order). Nothing for an element that isn't a MIDILINKS: whole state writes one while the song
// holds a link, none for none (SongState.h).
bool leadsToMidiLinks (const juce::ValueTree& parent, const juce::ValueTree& child);
void writeMidiLinksIn (juce::ValueTree& element, const SongState&);

// The MIDI links' reader of a song's tree (SongState.h: readSongTree(), a line of it): its MIDILINKS into the state, as the
// song holds them (none where it has none); no tree changed. Any thread.
void readHeldMidiLinksIn (const juce::ValueTree& song, SongState&);
} // namespace phazite
