// SPDX-License-Identifier: AGPL-3.0-or-later
// The tempo map's area of the song's values (SongValues.h, whose recipe this follows): its parts, each
// of which a list of SongValues.h's or WholeState.h's asks in turn, a line for each area. Its value and the conversion of its
// elements are TempoMapValue.h's. It holds no IDs (IDsHeld.h). Message thread, but for whole state's parts, which read only
// the values they are given.
#pragma once

#include "model/base/internal/SongValues.h"
#include "model/base/internal/TempoMapValue.h"
#include "model/base/internal/WholeState.h"

#include <juce_data_structures/juce_data_structures.h>

namespace phazite
{
class Song;

// What the values' listeners are told of the tempo map (Listener::tempoMapChanged()): once for each change of its value. It
// listens to the tempo map's area from its making to its end.
struct SongValues::TempoMapTeller final : ValueArea<TempoMapValue>::Listener
{
    TempoMapTeller (ValueArea<TempoMapValue>&, juce::ListenerList<SongValues::Listener>&);
    ~TempoMapTeller() override;

    ValueArea<TempoMapValue>& area;
    juce::ListenerList<SongValues::Listener>& listeners;

    void valueAreaChanged (const TempoMapValue& before, const TempoMapValue& after) override;

    JUCE_DECLARE_NON_COPYABLE (TempoMapTeller)
};

// The tempo map's part of whole state (WholeState.cpp: writeIn()): whether a child of an element in a copy of the song is the
// song's TEMPOMAP (the child of its PHAZITE element); and a copy's TEMPOMAP given the map kept (TempoMapValue.h:
// writeTempoMap(): tempos then time signatures, each kind by beat). Nothing for an element that isn't a TEMPOMAP.
bool leadsToTempoMap (const juce::ValueTree& parent, const juce::ValueTree& child);
void writeTempoMapIn (juce::ValueTree& element, const SongState&);

// The tempo map's reader of a song's tree (SongState.h: readSongTree(), a line of it): its TEMPOMAP's entries into the state,
// as the song holds them (an empty TEMPOMAP's where it has none); no tree changed. Any thread.
void readHeldTempoMapIn (const juce::ValueTree& song, SongState&);
} // namespace phazite
