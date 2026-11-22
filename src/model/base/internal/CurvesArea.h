// SPDX-License-Identifier: AGPL-3.0-or-later
// Automation's area of the song's values (roadmap step S5.20; SongValues.h, whose recipe this follows), as far as it is
// built: the curves are still the song's tree's (Schema.h: AUTOMATION, its CURVE children and their POINTs), and their
// values and the conversion are CurveValues.h's. Here: the curves' part of the keeper, which tells the values' listeners
// what the tree tells under the curves (Listener::curvesChanged()), and the curves' comparisons, which read the trees'
// curves into values until the switch. Message thread, but for the comparisons, which read only the trees and the values
// they are given.
#pragma once

#include "model/base/internal/CurveValues.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"
#include "model/devices/ParameterRef.h"

#include <juce_data_structures/juce_data_structures.h>

#include <utility>
#include <vector>

namespace phazite
{
class Song;

// The curves' part of the keeper (SongValues.h), while the song's tree holds the curves: for each of the tree's
// notifications under them (isHeardAsCurves()), whatever the history does (an undo's and a redo's, a write's outside the
// history), the values' listeners told the parameters of the curves it changed (Listener::curvesChanged()), as they are
// now and as they were: a POINT's or a CURVE's property, a child added to a CURVE, taken out of it or moved in it, that
// curve's; a CURVE added to the song's AUTOMATION or taken out, its own; a CURVE's own parameter changed (its device or its
// parameter: model/mixer/Buses.cpp gives a folder's curve to the fader that takes its place so), the one it had and the one
// it has; and the song's AUTOMATION added or taken out, each of its curves'. The song's AUTOMATION itself is the tree's: its
// own properties, a child of it that is no CURVE, and its children in another order tell nothing here. Nothing of the
// song's values is read, nor anything of the song's tree but the curves' elements' parameters.
struct SongValues::CurvesKeeper
{
    CurvesKeeper (SongValues&, Song&);

    // The keeper's callbacks, each for every element, whoever's it is, in an undo or a redo too: nothing, at once, but for
    // what is the curves'.
    void propertyChanged (const juce::ValueTree& tree, const juce::Identifier& property);
    void childAdded (const juce::ValueTree& parent, const juce::ValueTree& child);
    void childRemoved (const juce::ValueTree& parent, const juce::ValueTree& child);
    void childOrderChanged (const juce::ValueTree& parent);

    SongValues& values;
    juce::ValueTree state;

    // The song's curves' elements with the parameter each had as it was last told, which JUCE doesn't say as a property
    // changes: so that a curve given another parameter tells the one it had. Each CURVE child of the song's AUTOMATION once,
    // in no order; few, and looked through only as a curve comes, goes or changes its parameter.
    std::vector<std::pair<juce::ValueTree, ParameterRef>> told;

    void remember (const juce::ValueTree& curve);
    ParameterRef forget (const juce::ValueTree& curve); // the parameter it had
    void tell (std::vector<ParameterRef> parameters) const;

    JUCE_DECLARE_NON_COPYABLE (CurvesKeeper)
};

// The curves' part of the comparison of two elements with what their songs kept (WholeState.h: holdSameKeptOf()): two
// AUTOMATION elements hold the same curves, as values (holdSameCurves()), and any other element is no matter here; and
// whether a child is a curve's place, which the comparison of the elements in them leaves out (a CURVE in an AUTOMATION).
bool holdSameCurvesOf (const juce::ValueTree& element, const ValuesKept&, const juce::ValueTree& otherElement, const ValuesKept&);
bool isCurvesPlace (const juce::ValueTree& parent, const juce::ValueTree& child);
} // namespace phazite
