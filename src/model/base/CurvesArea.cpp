// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/CurvesArea.h"
#include "Song.h"
#include "internal/Schema.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    // The song's AUTOMATION (the child of its PHAZITE element), and a CURVE in it.
    bool isSongAutomation (const juce::ValueTree& element)
    {
        return element.hasType (schema::AUTOMATION) && element.getParent().hasType (schema::PHAZITE);
    }

    bool isSongCurve (const juce::ValueTree& element)
    {
        return element.hasType (schema::CURVE) && isSongAutomation (element.getParent());
    }

    // The song's CURVE an element is, or is in; an invalid tree for one in none.
    juce::ValueTree findSongCurve (const juce::ValueTree& element)
    {
        for (auto each = element; each.isValid(); each = each.getParent())
            if (isSongCurve (each))
                return each;

        return {};
    }

    // A tree's AUTOMATION's curves' parameters, in its order, each once (the first of one parameter, as readCurvesValue()
    // reads them), without reading their points.
    std::vector<ParameterRef> readCurveOrder (const juce::ValueTree& automation)
    {
        std::vector<ParameterRef> order;

        for (const auto& child : automation)
            if (child.hasType (schema::CURVE))
                if (const auto parameter = readCurveParameter (child); std::find (order.begin(), order.end(), parameter) == order.end())
                    order.push_back (parameter);

        return order;
    }

    // A parameter's CURVE in a tree's AUTOMATION, the first of that parameter; an invalid tree for none.
    juce::ValueTree findCurveElement (const juce::ValueTree& automation, const ParameterRef& parameter)
    {
        for (const auto& child : automation)
            if (child.hasType (schema::CURVE) && readCurveParameter (child) == parameter)
                return child;

        return {};
    }

    // Two AUTOMATION elements' curves, as values: the same parameters in the same order, each curve's points alike. While the
    // trees hold the curves, both read into values (the switch of roadmap step S5.20 compares the values kept).
    bool holdSameCurvesIn (const juce::ValueTree& automation, const juce::ValueTree& otherAutomation)
    {
        return readCurvesValue (automation) == readCurvesValue (otherAutomation);
    }
} // namespace

//==============================================================================
SongValues::CurvesKeeper::CurvesKeeper (SongValues& v, Song& s)
    : values (v),
      state (s.getState())
{
    // The curves the song holds as the keeper starts (SongValues.cpp: adopt()), each with its parameter.
    for (const auto& curve : state.getChildWithName (schema::AUTOMATION))
        if (curve.hasType (schema::CURVE))
            remember (curve);
}

void SongValues::CurvesKeeper::remember (const juce::ValueTree& curve)
{
    const auto parameter = readCurveParameter (curve);

    for (auto& [element, known] : told)
    {
        if (element == curve)
        {
            known = parameter;
            return;
        }
    }

    told.emplace_back (curve, parameter);
}

ParameterRef SongValues::CurvesKeeper::forget (const juce::ValueTree& curve)
{
    const auto found = std::find_if (told.begin(), told.end(), [&curve] (const auto& each) { return each.first == curve; });

    if (found == told.end())
        return readCurveParameter (curve);

    auto parameter = std::move (found->second);
    told.erase (found);
    return parameter;
}

void SongValues::CurvesKeeper::tell (std::vector<ParameterRef> parameters) const
{
    // Each once, in the order they came.
    std::vector<ParameterRef> once;
    once.reserve (parameters.size());

    for (auto& parameter : parameters)
        if (std::find (once.begin(), once.end(), parameter) == once.end())
            once.push_back (std::move (parameter));

    if (! once.empty())
        values.listeners.call ([&once] (SongValues::Listener& listener) { listener.curvesChanged (once); });
}

void SongValues::CurvesKeeper::propertyChanged (const juce::ValueTree& tree, const juce::Identifier& property)
{
    if (! isHeardAsCurves (tree))
        return;

    // A curve given another parameter: the one it had (as it was last told) and the one it has.
    if (isSongCurve (tree) && (property == schema::curveDevice || property == schema::curveParameter))
    {
        const auto was = forget (tree);
        remember (tree);
        return tell ({ was, readCurveParameter (tree) });
    }

    tell ({ readCurveParameter (findSongCurve (tree)) });
}

void SongValues::CurvesKeeper::childAdded (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    // The song's AUTOMATION added, with its curves.
    if (child.hasType (schema::AUTOMATION) && parent == state)
    {
        std::vector<ParameterRef> added;

        for (const auto& curve : child)
        {
            if (curve.hasType (schema::CURVE))
            {
                remember (curve);
                added.push_back (readCurveParameter (curve));
            }
        }

        return tell (std::move (added));
    }

    if (! isHeardAsCurves (parent, child))
        return;

    // A curve added; or a point (or anything) added to one.
    if (isSongCurve (child))
    {
        remember (child);
        return tell ({ readCurveParameter (child) });
    }

    tell ({ readCurveParameter (findSongCurve (parent)) });
}

void SongValues::CurvesKeeper::childRemoved (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    // The song's AUTOMATION taken out, with its curves.
    if (child.hasType (schema::AUTOMATION) && parent == state)
    {
        std::vector<ParameterRef> taken;

        for (const auto& curve : child)
            if (curve.hasType (schema::CURVE))
                taken.push_back (forget (curve));

        return tell (std::move (taken));
    }

    if (! isHeardAsCurves (parent, child))
        return;

    // A curve taken out (no longer in the song: its parameter as it was last told); or a point taken out of one.
    if (child.hasType (schema::CURVE) && isSongAutomation (parent))
        return tell ({ forget (child) });

    tell ({ readCurveParameter (findSongCurve (parent)) });
}

void SongValues::CurvesKeeper::childOrderChanged (const juce::ValueTree& parent)
{
    // Points moved in a curve. (Curves moved in the song's AUTOMATION are the tree's: isHeardAsCurves().)
    if (isHeardAsCurves (parent))
        tell ({ readCurveParameter (findSongCurve (parent)) });
}

//==============================================================================
bool isHeardAsCurves (const juce::ValueTree& element)
{
    return findSongCurve (element).isValid();
}

bool isHeardAsCurves (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return (child.hasType (schema::CURVE) && isSongAutomation (parent)) || isHeardAsCurves (parent);
}

//==============================================================================
bool holdSameCurve (const juce::ValueTree& song, const ValuesKept&, const juce::ValueTree& otherSong, const ValuesKept&,
                    const ParameterRef& parameter)
{
    // The trees hold the curves still: each read into its value. (The switch compares the values kept.)
    const auto curve = findCurveElement (song.getChildWithName (schema::AUTOMATION), parameter);
    const auto other = findCurveElement (otherSong.getChildWithName (schema::AUTOMATION), parameter);

    if (curve.isValid() != other.isValid())
        return false;

    return ! curve.isValid() || readCurveValue (curve) == readCurveValue (other);
}

bool holdSameCurveOrder (const juce::ValueTree& song, const ValuesKept&, const juce::ValueTree& otherSong, const ValuesKept&)
{
    const auto automation = song.getChildWithName (schema::AUTOMATION);
    const auto other = otherSong.getChildWithName (schema::AUTOMATION);
    return automation.isValid() == other.isValid() && readCurveOrder (automation) == readCurveOrder (other);
}

bool holdSameCurves (const juce::ValueTree& song, const ValuesKept&, const juce::ValueTree& otherSong, const ValuesKept&)
{
    const auto automation = song.getChildWithName (schema::AUTOMATION);
    const auto other = otherSong.getChildWithName (schema::AUTOMATION);
    return automation.isValid() == other.isValid() && holdSameCurvesIn (automation, other);
}

bool holdSameCurvesOf (const juce::ValueTree& element, const ValuesKept&, const juce::ValueTree& otherElement, const ValuesKept&)
{
    if (! element.hasType (schema::AUTOMATION) || ! otherElement.hasType (schema::AUTOMATION))
        return true;

    return holdSameCurvesIn (element, otherElement);
}

bool isCurvesPlace (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return parent.hasType (schema::AUTOMATION) && child.hasType (schema::CURVE);
}
} // namespace phazite
