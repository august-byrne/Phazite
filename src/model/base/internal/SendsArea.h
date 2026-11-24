// SPDX-License-Identifier: AGPL-3.0-or-later
// The sends' area of the song's values (roadmap step S5.20; SongValues.h, whose recipe this follows): its parts, each of which
// a list of SongValues.h's, WholeState.h's or IDsHeld.h's asks in turn, a line for each area. Its value and the conversion of
// its elements are SendsValue.h's. It holds IDs (each send's). Message thread, but for whole state's parts and the
// comparisons, which read only the values they are given.
//
// An area under an element that stays in the song's tree, a TRACK, as the clips are: so its keeper follows a track as it
// comes and goes (alone, a folder with its tracks, the track list), as the clips' keeper does; no other element holds a send,
// and no edit moves one to another track (a version's pass sets its track: model/versions/ApplySong.cpp, makeSendsLike()).
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/SendsValue.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>

namespace phazite
{
class Song;

// What the values' listeners are told of the sends (Listener::sendsChanged()): once for each change of its value, each send
// whose value differs between the two (immer's diff, which skips what the two share), with the track it was on and is on,
// the return it went to and goes to, the parts of it that changed and its value after; each track whose list holds those it
// held before in another order; and the area's whole value after (cheap: it shares what it holds), which what plays keeps
// for what decides which send sends (SongPlayer.h). It listens to the sends' area from its making to its end.
struct SongValues::SendsTeller final : ValueArea<SongSends>::Listener
{
    SendsTeller (ValueArea<SongSends>&, juce::ListenerList<SongValues::Listener>&);
    ~SendsTeller() override;

    ValueArea<SongSends>& area;
    juce::ListenerList<SongValues::Listener>& listeners;

    void valueAreaChanged (const SongSends& before, const SongSends& after) override;

    JUCE_DECLARE_NON_COPYABLE (SendsTeller)
};

// The sends' adoption from the loaded tree, which the song's holdValues() asks: every send of the tree the loader made
// consistent into the area, outside the history, as the song holds it (each track's in its order, read by readHeldSend(),
// each of which the loader left as the song holds it: asserted), and every SEND element out of the tree, those anywhere else
// too (SongValues.cpp: adopt()). The keeper asks the sends of nothing: no track is in a live song's tree for a SEND to come
// into it on (the tracks' safety net reads a TRACK's: takeTrackSendsIn(), below).
struct SongValues::SendsKeeper
{
    static void adopt (SongValues& into, Song& loaded);
};

// The sends' part of whole state (WholeState.cpp: writeIn()): a copy's TRACK given its sends from the values kept, by its ID,
// in their list's order, after its other children (its modulators and macros among them, which modulation's part writes
// before: decision L.5 of roadmap step S5.20's design). Nothing for another element. Whole state writes each TRACK itself
// (WholeState.cpp: writeTracksIn()), asking this of each.
void writeSendsIn (juce::ValueTree& element, const ValuesKept&);

// The SEND elements on a track's element (one that comes into the song with them: the tracks' safety net, TracksArea.h), each
// read as the song holds one (SendsValue.h: readHeldSend()) from the track of this ID, after its others; one of an ID the
// sends hold already left out. How many it read.
int takeTrackSendsIn (SongSends&, const juce::ValueTree& track, TrackID);

// The sends' part of the comparison of two elements with what their songs kept (WholeState.h: holdSameKeptOf()): a TRACK's
// sends, the same in the same order, each the same value; true for another element.
bool holdSameSendsOf (const juce::ValueTree& element, const ValuesKept&, const juce::ValueTree& otherElement, const ValuesKept&);
} // namespace phazite
