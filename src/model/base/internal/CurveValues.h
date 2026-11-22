// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's automation as values (roadmap step S5.20, automation's area): a point of a parameter's curve, a curve with its
// points in their order, and the song's curves by the parameter each automates, in the order the song made them; and the
// conversion between those values and the AUTOMATION element with its CURVE and POINT children (Schema.h), the one place
// that reads and writes those by name. The song doesn't hold these values yet: its curves are still its tree's elements,
// which automation's code (model/automation/internal/Automation.h: getCurvesValue(), getCurveKeys(), getCurveValue())
// reads into them, so that what reads the song's automation reads it as values, whatever stores it.
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
// POINT without a curve holds).
struct PointValue
{
    BeatPosition beat;
    float position = 0.0f;
    float curve = 0.0f;

    // Written out, not defaulted: a defaulted one would compare the floats with a floating-point ==, which GCC's -Wfloat-equal
    // refuses.
    bool operator== (const PointValue&) const noexcept;
};

// A parameter's curve: its points in the curve's order (time order as Phazite writes them, two at one beat a jump, in the
// order they go; a file from others may hold them out of time order, which is kept). Copies are cheap and share what they
// hold.
struct CurveValue
{
    immer::flex_vector<PointValue> points;

    // The same value without a look inside: its points shared (ValueArea.h). And the same points, compared.
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

// A curve's value read from its CURVE element (an invalid tree reads as one without points): its POINT children in the
// element's order, each property as automation's code has always read it (a beat that is absent reads as 0, a curve as 0;
// nothing kept within a range here, which the loader does: model/project/internal/Project.h, makeConsistent()); a point
// without a value is no point, and is left out (the loader takes one out as the song opens). A property Phazite doesn't
// know is no part of it, nor a child (the loader takes such a child out as the song opens).
CurveValue readCurveValue (const juce::ValueTree& curve);

// How many points were read into curves' values (readCurveValue()) since Phazite started, on any thread: for the test that
// an edit of several points of a curve reads the curve once, however many they are (tests/PerformanceGateTests.cpp).
// Allocations can't tell it: immer takes its nodes from lists of freed ones.
std::uint64_t countCurvePointsRead() noexcept;

// Whether a CURVE element holds a point readCurveValue() would read, without reading them all.
bool holdsPoints (const juce::ValueTree& curve);

// The song's curves read from its AUTOMATION element (an invalid tree reads as none): each CURVE child in the element's
// order, one for each parameter (the first, should the element hold two: a tree not made consistent; the loader keeps the
// first of one parameter, its device's ID read as a number, as here: makeConsistent()). A child of another type is no curve.
SongCurves readCurvesValue (const juce::ValueTree& automation);

// A curve appended to an AUTOMATION element (a copy of the song's, emptied, outside any song: WholeState.h) as a CURVE, as
// automation's code writes one: its device's ID as text ("1042") and its parameter; then its POINTs, each with its beat
// and value, and its curve only while it isn't 0. And the song's curves, each so, in their order.
void writeCurveElement (const ParameterRef&, const CurveValue&, juce::ValueTree& automationCopy);
void writeCurves (const SongCurves&, juce::ValueTree& automationCopy);
} // namespace phazite
