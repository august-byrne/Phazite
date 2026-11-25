// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's automation as values: a point of a parameter's curve, a curve with its points in their order, and the song's
// curves by the parameter each automates, in the order the song made them; and the conversion between those values and the
// AUTOMATION element with its CURVE and POINT children (Schema.h), the one place that reads and writes those by name. The
// song holds its curves as one such value in an area of its values (SongValues.h: SongValues::curves, CurvesArea.h), which
// the song's file form writes into an AUTOMATION while it has any (SongState.h: writeSongTree()); automation's code
// (model/automation/internal/Automation.h) reads and changes them.
//
// Its own header, beside SongValues.h and TempoMapValue.h (one header for each area's values): the model's base includes no
// other area, but a parameter by its IDs (model/devices/ParameterRef.h), as Selection.h does. immer is the model's alone
// (tools/check-architecture.sh: immer-outside-model).
#pragma once

#include "model/base/Beats.h"
#include "model/base/internal/Immer.h"
#include "model/devices/ParameterRef.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace phazite
{
// A point as a curve holds it (Schema.h: POINT): its beat, its position along its parameter's control's travel (0 to 1,
// model/automation/AutomationValues.h: AutomationPoint), and the bend of the line to the next (-1 to 1, 0 straight: what a
// POINT without a curve holds); and its key, which tells it from every other point of the song while the song is open, so
// that the selection of points follows it through edits, undo and redo (model/base/internal/Selection.h). A key is never
// saved nor written into an element, and comes from the song's values (SongValues.h: givePointKey()), outside the undo
// history: each point the song takes in or makes gets a new one, and an edit that keeps a point keeps its key
// (model/automation/Automation.cpp). 0 is none: a point read for reading alone (a version's, a file's), which no song holds.
struct PointValue
{
    BeatPosition beat;
    float position = 0.0f;
    float curve = 0.0f;
    std::uint64_t key = 0;

    // The same point as saved: its beat, its position and its bend, each exactly; its key aside, which is no part of what the
    // song holds as content (two curves alike are alike whatever their points' keys, as a file would hold them). Written
    // out, not defaulted: a defaulted one would compare the floats with a floating-point ==, which GCC's -Wfloat-equal
    // refuses, and the keys too.
    bool operator== (const PointValue&) const noexcept;
};

// A parameter's curve: its points in the curve's order (time order as Phazite writes them, two at one beat a jump, in the
// order they go; a file from others may hold them out of time order, which is kept). Copies are cheap and share what they
// hold.
struct CurveValue
{
    immer::flex_vector<PointValue> points;

    // The same value without a look inside: its points shared (ValueArea.h). And the same points, compared as saved (their
    // keys aside: PointValue's ==), counted (countCurvePointsRead()) where they aren't shared.
    bool isSameAs (const CurveValue& other) const noexcept { return points.identity() == other.points.identity(); }
    bool operator== (const CurveValue&) const;
};

// A parameter as the key of a map: its device's ID and its parameter's text.
struct ParameterRefHash
{
    std::size_t operator() (const ParameterRef&) const noexcept;
};

// Every curve of the song (Schema.h: AUTOMATION): by the parameter it automates, one for each, and the order the song made
// them in, which is the file's order and the order a plug-in's parameters' lanes show in (model/automation/internal/
// Automation.h: getLanes()). Only the helpers below write one, so that the map and the order hold the same parameters.
struct SongCurves
{
    immer::map<ParameterRef, CurveValue, ParameterRefHash> curves;
    immer::flex_vector<ParameterRef> order;

    const CurveValue* find (const ParameterRef& parameter) const { return curves.find (parameter); }

    // The same value without a look inside: both shared. And the same curves, alike, in the same order.
    bool isSameAs (const SongCurves&) const noexcept;
    bool operator== (const SongCurves&) const;
};

// The song's curves with a parameter's curve set: changed in its place, or added after the others; and without it.
SongCurves withCurve (const SongCurves&, const ParameterRef&, CurveValue);
SongCurves withoutCurve (const SongCurves&, const ParameterRef&);

// The same curve under another parameter, in its place in the order (a fader's curve given to the fader that takes its
// place: model/mixer/Buses.cpp). The other parameter has no curve of its own (asserted).
SongCurves withCurveRenamed (const SongCurves&, const ParameterRef& from, const ParameterRef& to);

// The song's curves with the curves of these parameters (each once) first in the order, in this order, the others after them
// in theirs; a parameter without a curve left out. The same value where that is their order already.
SongCurves withCurveOrder (const SongCurves&, const std::vector<ParameterRef>& first);

// Whether the order holds each parameter once, and the map the same parameters: for assertions and tests.
bool holdsItsOrder (const SongCurves&);

// The text the versions name a curve by (Schema.h: getElementKey() of a CURVE: its device's ID, "/", its parameter:
// "1042/cutoff"), and the parameter such a text names: the device's digits before the first "/" (read as readID() reads an
// ID), the parameter's text after it; none for a text without a "/".
juce::String getCurveKeyText (const ParameterRef&);
std::optional<ParameterRef> readCurveKeyText (const juce::String&);

//==============================================================================
// The conversion, the one place that reads and writes CURVE and POINT by name.

// The parameter a CURVE automates, as the song's code has always read it: its device's ID read as readID() reads one (text
// or a number), its parameter's text.
ParameterRef readCurveParameter (const juce::ValueTree& curve);

// A curve's value read from its CURVE element as the song holds a curve (an invalid tree reads as one without points;
// Schema.h's descriptions: SongText.h, readDescribed()): each point's beat within 0 and the longest song (0 where it has
// none), its value within 0 and 1, its bend within -1 and 1 (0 where it has none), in the element's order; a point without a
// value that is a finite number left out; what Phazite doesn't know of the curve and its points (a property, a child that is
// no point) no part of it. Each point given the next key from nextKey where it is given (SongValues.h: the song's point keys,
// which the song made of its state gives: CurvesArea.h), else none. What enters the song's curves reads a curve so: the
// song's own as it opens (SongState.h: readSongTree()), a version's (model/versions/ApplySong.cpp: makeCurvesLike()) and a
// merge's choice (model/automation/internal/Automation.h: chooseCurveClash()).
CurveValue readHeldCurve (const juce::ValueTree& curve, std::uint64_t* nextKey = nullptr);

// The song's curves read from its AUTOMATION as the song holds them (readHeldCurve()), each CURVE child in the element's
// order, one for each parameter (the first; the loader keeps the first: makeConsistent()), each point given a key from
// nextKey; a child of another type no curve; an invalid tree reads as none.
SongCurves readHeldCurves (const juce::ValueTree& automation, std::uint64_t& nextKey);

// How many points were read into curves' values (readHeldCurve()) or compared one by one with another curve's (CurveValue's
// ==, where the two aren't shared) since Phazite started, on any thread: for the tests that an edit of several points of a
// curve reads the curve once, however many they are (tests/PerformanceGateTests.cpp), and that a frozen track's curves are
// compared only where one changed (tests/FreezeTests.cpp). Allocations can't tell it: immer takes its nodes from lists of
// freed ones.
std::uint64_t countCurvePointsRead() noexcept;

// A curve appended to an AUTOMATION element (outside any song: SongState.h, writeSongTree()) as a CURVE, as automation's code
// writes one: its device's ID as text ("1042") and its parameter; then its POINTs, each with its beat and value, and its
// curve only while it isn't 0 (a point's key never). And the song's curves, each so, in their order.
void writeCurveElement (const ParameterRef&, const CurveValue&, juce::ValueTree& automationCopy);
void writeCurves (const SongCurves&, juce::ValueTree& automationCopy);
} // namespace phazite
