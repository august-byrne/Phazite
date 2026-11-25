// SPDX-License-Identifier: AGPL-3.0-or-later
// Automation's area of the song's values (SongValues.h, whose recipe this follows): its parts, each of
// which a list of SongValues.h's or WholeState.h's asks in turn, a line for each area. Its value and the conversion of its
// elements are CurveValues.h's. It holds no IDs of its own (a curve names a device's: IDsHeld.h has nothing of it). Message
// thread, but for whole state's parts, which read only the values they are given.
#pragma once

#include "model/base/internal/CurveValues.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>

namespace phazite
{
class Song;

// What the values' listeners are told of the curves (Listener::curvesChanged()): once for each change of the area's value,
// the parameters whose curves differ by identity between the value left and the value come to (one added, taken out, or
// with other points; a curve given another parameter tells the one it had and the one it has), and those of the curves both
// hold, the same, whose places among them changed (the order alone set: setCurveOrder()). It listens to the curves' area
// from its making to its end.
struct SongValues::CurvesTeller final : ValueArea<SongCurves>::Listener
{
    CurvesTeller (ValueArea<SongCurves>&, juce::ListenerList<SongValues::Listener>&);
    ~CurvesTeller() override;

    ValueArea<SongCurves>& area;
    juce::ListenerList<SongValues::Listener>& listeners;

    void valueAreaChanged (const SongCurves& before, const SongCurves& after) override;

    JUCE_DECLARE_NON_COPYABLE (CurvesTeller)
};

// The curves' part of whole state (WholeState.cpp: writeIn()): whether a child of an element in a copy of the song is the
// song's AUTOMATION (the child of its PHAZITE element); and a copy's AUTOMATION given the curves kept (CurveValues.h:
// writeCurves(), in their order). Whole state writes an AUTOMATION while the song holds a curve alone (SongState.h), so that a
// song without curves is written as one whose file never had any. Nothing for an element that isn't an AUTOMATION.
bool leadsToCurves (const juce::ValueTree& parent, const juce::ValueTree& child);
void writeCurvesIn (juce::ValueTree& element, const SongState&);

// The curves' reader of a song's tree (SongState.h: readSongTree(), a line of it): its AUTOMATION's curves into the state, as
// the song holds them, each point given a key from the count given (none where it has none); no tree changed. Any thread.
void readHeldCurvesIn (const juce::ValueTree& song, SongState&, std::uint64_t& nextPointKey);
} // namespace phazite
