// SPDX-License-Identifier: AGPL-3.0-or-later
// The sends' area of the song's values (SongValues.h, whose recipe this follows): its parts, each of which
// a list of SongValues.h's, WholeState.h's or IDsHeld.h's asks in turn, a line for each area. Its value and the conversion of
// its elements are SendsValue.h's. It holds IDs (each send's). Message thread, but for whole state's parts and the
// comparisons, which read only the values they are given.
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

// The sends' part of whole state (WholeState.cpp: writeIn()): a copy's TRACK given its sends from the values kept, by its ID,
// in their list's order, after its other children (its modulators and macros among them, which modulation's part writes
// before). Nothing for another element. Whole state writes each TRACK itself
// (WholeState.cpp: writeTracksIn()), asking this of each.
void writeSendsIn (juce::ValueTree& element, const SongState&);

// The sends' reader of a song's tree (SongState.h: readSongTree(), a line of it): every send of its tracks into the state, as
// the song holds them; no tree changed. Any thread.
void readHeldSendsIn (const juce::ValueTree& song, SongState&);
} // namespace phazite
