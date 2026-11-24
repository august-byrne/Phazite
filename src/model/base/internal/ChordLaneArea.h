// SPDX-License-Identifier: AGPL-3.0-or-later
// The chord lane's area of the song's values (roadmap step S5.20; SongValues.h, whose recipe this follows): its parts, each
// of which a list of SongValues.h's or WholeState.h's asks in turn, a line for each area. Its value and the conversion of its
// elements are ChordLaneValue.h's. It holds no IDs (IDsHeld.h). Message thread, but for whole state's parts, which read only
// the values they are given.
//
// The lane's CHORDLANE element stays in the song's tree, empty, as the sign that the song has a lane (a song without one
// takes its keys from the key set last: model/chords/internal/Chords.h, hasChordLane()), and the place a whole copy writes the
// lane's chords and keys in. Who makes and takes away the element is the chord lane's code (Chords.h: the writer that makes
// the lane's first chord or key, in that step; and a version's lane or a merge's choice made the song's, makeChordLaneAs()),
// never a reader; the element's own coming and going is the tree's, which its listeners hear as before.
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

// The chord lane's part of the keeper (SongValues.h), its safety net: a CHORDLANE that comes into the song with chords or keys
// in it has them read in as the song's lane (readHeldChordLane()), and is emptied as the adoption empties the song's own
// (what Phazite doesn't know of it goes too); a CHORD or a CHORDKEY added to the song's CHORDLANE by itself is read into the
// lane and taken out again while the song is written outside the history, and refused in a step. Nothing when one of its
// elements leaves the tree. And the chord lane's adoption from the loaded tree, which the song's holdValues() asks.
struct SongValues::ChordLaneKeeper
{
    ChordLaneKeeper (SongValues&, Song&);

    // A child added to an element of the song's tree (the keeper's callback, but while the history undoes or redoes, for
    // every element, whoever's it is): nothing, at once, but for the song's CHORDLANE or a child added to it.
    void childAdded (juce::ValueTree& parent, juce::ValueTree& child);

    // The song's CHORDLANE emptied, once its reader read the lane the loader made consistent into the area
    // (readHeldChordLaneIn(), SongValues.cpp: adopt()). A song without a CHORDLANE keeps none, its lane empty and not placed.
    static void adopt (SongValues& into, Song& loaded);

    SongValues& values;
    Song& song;
    juce::ValueTree state;

    JUCE_DECLARE_NON_COPYABLE (ChordLaneKeeper)
};

// The chord lane's part of whole state (WholeState.cpp: writeIn()): whether a child of an element in a copy of the song is the
// song's CHORDLANE (the child of its PHAZITE element); and a copy's CHORDLANE given the lane kept (ChordLaneValue.h:
// writeChordLane(): chords then keys, each kind in its order), where one is kept (a song that keeps everything in its tree
// keeps an empty lane: its CHORDLANE holds its chords and keys). Nothing for an element that isn't a CHORDLANE.
bool leadsToChordLane (const juce::ValueTree& parent, const juce::ValueTree& child);
void writeChordLaneIn (juce::ValueTree& element, const SongState&);

// The chord lane's reader of a song's tree (SongState.h: readSongTree(), a line of it): its CHORDLANE into the state, as the
// song holds it, placed where the tree has one (ChordLaneValue.h); no tree changed. Any thread.
void readHeldChordLaneIn (const juce::ValueTree& song, SongState&);

// The chord lane's part of the comparison of two elements with what their songs kept (WholeState.h: holdSameKeptOf()): where
// either is a song's element that holds a CHORDLANE, the same lane (holdSameChordLane()); and whether a child is the place of
// the lane, which the comparison of the elements in them leaves out (the song's CHORDLANE).
bool holdSameChordLaneOf (const juce::ValueTree& element, const SongState&, const juce::ValueTree& otherElement, const SongState&);
bool isChordLanePlace (const juce::ValueTree& parent, const juce::ValueTree& child);
} // namespace phazite
