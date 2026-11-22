// SPDX-License-Identifier: AGPL-3.0-or-later
// Automation's area of the song's values (roadmap step S5.20; SongValues.h, whose recipe this follows): its parts, each of
// which a list of SongValues.h's or WholeState.h's asks in turn, a line for each area. Its value and the conversion of its
// elements are CurveValues.h's. It holds no IDs of its own (a curve names a device's: IDsHeld.h has nothing of it). Message
// thread, but for whole state's parts, which read only the values they are given.
//
// The song's AUTOMATION stays in its tree, empty, a place every song that holds its values has (made as the song holds them,
// where its file had none: adopt(), below), as its TEMPOMAP is: the place a whole copy writes the curves in, which never comes
// or goes in a song that holds its values, and which what listens to the song's root leaves alone whole (isHeardAsCurves()).
// A whole copy writes it only while the song has a curve (writeCurvesIn(), below), so that a song without curves is written
// as one whose file never had one was: without an AUTOMATION.
#pragma once

#include "model/base/internal/CurveValues.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"

#include <juce_data_structures/juce_data_structures.h>

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

// The curves' part of the keeper (SongValues.h), its safety net: a CURVE added to the song's AUTOMATION by itself is read in
// (its parameter's curve set: a curve the song has of it replaced, each point given a key) and taken out again while the song
// is written outside the history, and refused in a step; an AUTOMATION added to the song, a second (the song has its own from
// the adoption on), refused in either mode, its curves left unread: taken out again outside the history, left to the step's
// Undo in a step. Nothing when one of its elements leaves the tree. And the curves' adoption from the loaded tree, which the
// song's holdValues() asks.
struct SongValues::CurvesKeeper
{
    CurvesKeeper (SongValues&, Song&);

    // A child added to an element of the song's tree (the keeper's callback, but while the history undoes or redoes, for
    // every element, whoever's it is): nothing, at once, but for an AUTOMATION added to the song or a child added to its own.
    void childAdded (juce::ValueTree& parent, juce::ValueTree& child);

    // The curves the loader made consistent into their area, outside the history, each point given a key, and their element
    // emptied (SongValues.cpp: adopt()); a song whose file had no AUTOMATION given one, empty.
    static void adopt (SongValues& into, Song& loaded);

    SongValues& values;
    Song& song;
    juce::ValueTree state;

    JUCE_DECLARE_NON_COPYABLE (CurvesKeeper)
};

// The curves' part of whole state (WholeState.cpp: writeIn()): whether a child of an element in a copy of the song is the
// song's AUTOMATION (the child of its PHAZITE element); and a copy's AUTOMATION given the curves kept (CurveValues.h:
// writeCurves(), in their order), where some are kept (a song that keeps everything in its tree keeps none: its AUTOMATION
// holds its curves). And a copy of the song's element whose AUTOMATION is left empty, its song having no curve, without it:
// a song without curves is written as one whose file never had any (the AUTOMATION a song that holds its values always
// has is its place alone). Nothing for an element that isn't the song's or its AUTOMATION.
bool leadsToCurves (const juce::ValueTree& parent, const juce::ValueTree& child);
void writeCurvesIn (juce::ValueTree& element, const ValuesKept&);

// The curves' part of the comparison of two elements with what their songs kept (WholeState.h: holdSameKeptOf()): where both
// are a song's element (PHAZITE), the same curves (holdSameCurves()); and whether a child is the place of the curves, which
// the comparison of the elements in them leaves out (the song's AUTOMATION).
bool holdSameCurvesOf (const juce::ValueTree& element, const ValuesKept&, const juce::ValueTree& otherElement, const ValuesKept&);
bool isCurvesPlace (const juce::ValueTree& parent, const juce::ValueTree& child);
} // namespace phazite
