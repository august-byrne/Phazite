// SPDX-License-Identifier: AGPL-3.0-or-later
// The chord lane's area of the song's values (roadmap step S5.20; SongValues.h, whose recipe this follows): its parts, each
// of which a list of SongValues.h's or WholeState.h's asks in turn, a line for each area. Its value and the conversion of its
// elements are ChordLaneValue.h's. It holds no IDs (IDsHeld.h). Message thread, but for whole state's parts, which read only
// the values they are given.
#pragma once

#include "model/base/internal/ChordLaneValue.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"

#include <juce_data_structures/juce_data_structures.h>

namespace phazite
{
class Song;

// What the values' listeners are told of the chord lane (Listener::chordLaneChanged()): once for each change of its value. It
// listens to the chord lane's area from its making to its end.
struct SongValues::ChordLaneTeller final : ValueArea<ChordLaneValue>::Listener
{
    ChordLaneTeller (ValueArea<ChordLaneValue>&, juce::ListenerList<SongValues::Listener>&);
    ~ChordLaneTeller() override;

    ValueArea<ChordLaneValue>& area;
    juce::ListenerList<SongValues::Listener>& listeners;

    void valueAreaChanged (const ChordLaneValue& before, const ChordLaneValue& after) override;

    JUCE_DECLARE_NON_COPYABLE (ChordLaneTeller)
};

// The chord lane's part of whole state (WholeState.cpp: writeIn()): whether a child of an element in a copy of the song is the
// song's CHORDLANE (the child of its PHAZITE element); and a copy's CHORDLANE given the lane kept (ChordLaneValue.h:
// writeChordLane(): chords then keys, each kind in its order). Nothing for an element that isn't a CHORDLANE. Whether the
// song has a lane is its value's (ChordLaneValue.h: placed), which whole state writes as a CHORDLANE, empty without entries.
bool leadsToChordLane (const juce::ValueTree& parent, const juce::ValueTree& child);
void writeChordLaneIn (juce::ValueTree& element, const SongState&);

// The chord lane's reader of a song's tree (SongState.h: readSongTree(), a line of it): its CHORDLANE into the state, as the
// song holds it, placed where the tree has one (ChordLaneValue.h); no tree changed. Any thread.
void readHeldChordLaneIn (const juce::ValueTree& song, SongState&);
} // namespace phazite
