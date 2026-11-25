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
    // Two entries of the curves' map alike without a look inside: one parameter's, the same value (ValueArea.h).
    struct SameCurve
    {
        bool operator() (const std::pair<ParameterRef, CurveValue>& a, const std::pair<ParameterRef, CurveValue>& b) const noexcept
        {
            return a.first == b.first && a.second.isSameAs (b.second);
        }
    };

    // The curves both values hold that aren't told already, in each value's order: the order alone changed where these
    // differ.
    std::vector<ParameterRef> findKept (const SongCurves& curves, const SongCurves& other, const std::vector<ParameterRef>& told)
    {
        std::vector<ParameterRef> kept;
        kept.reserve (curves.order.size());

        for (const auto& parameter : curves.order)
            if (other.find (parameter) != nullptr && std::find (told.begin(), told.end(), parameter) == told.end())
                kept.push_back (parameter);

        return kept;
    }
} // namespace

//==============================================================================
SongValues::CurvesTeller::CurvesTeller (ValueArea<SongCurves>& a, juce::ListenerList<SongValues::Listener>& l)
    : area (a),
      listeners (l)
{
    area.addListener (this);
}

SongValues::CurvesTeller::~CurvesTeller()
{
    area.removeListener (this);
}

void SongValues::CurvesTeller::valueAreaChanged (const SongCurves& before, const SongCurves& after)
{
    if (listeners.isEmpty() || before.isSameAs (after))
        return;

    // The curves that differ by identity (immer's map compared node by node: what both share isn't looked at), each once.
    std::vector<ParameterRef> told;
    const auto tell = [&told] (const std::pair<ParameterRef, CurveValue>& entry) { told.push_back (entry.first); };

    before.curves.impl().template diff<SameCurve> (
        after.curves.impl(),
        immer::make_differ (tell, tell,
                            [&told] (const std::pair<ParameterRef, CurveValue>&, const std::pair<ParameterRef, CurveValue>& entry)
                            { told.push_back (entry.first); }));

    // The order alone: those both hold that came to other places among them, each once.
    if (before.order.identity() != after.order.identity())
    {
        const auto was = findKept (before, after, told), is = findKept (after, before, told);

        for (size_t i = 0; i < was.size() && i < is.size(); ++i)
            if (was[i] != is[i])
                told.push_back (is[i]);
    }

    if (! told.empty())
        listeners.call ([&told] (SongValues::Listener& listener) { listener.curvesChanged (told); });
}

//==============================================================================
void readHeldCurvesIn (const juce::ValueTree& song, SongState& into, std::uint64_t& nextPointKey)
{
    // The curves the loader made consistent, each as the song holds one, one for each parameter, each point given a key from
    // the count given (CurveValues.h: readHeldCurves()); none for a song without an AUTOMATION, or with an empty one.
    const auto automation = song.getChildWithName (schema::AUTOMATION);
    into.curves = readHeldCurves (automation.isValid() ? automation : juce::ValueTree (schema::AUTOMATION), nextPointKey);
}

//==============================================================================
void SongValues::setCurves (Song& song, SongCurves next)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);
    jassert (holdsItsOrder (next));

    if (next.isSameAs (curves.get()) || next == curves.get())
        return;

    if (song.isWritingOutsideHistory())
        curves.setOutsideHistory (std::move (next));
    else
        curves.set (std::move (next));
}

CurveValue SongValues::keyByPlace (const CurveValue* held, CurveValue curve)
{
    // At each place both have, the key of the point the selection may hold (the rule of what an edit keeps:
    // model/automation/Automation.cpp); past the held curve's last, new keys.
    auto points = curve.points.transient();
    const auto numHeld = held != nullptr ? held->points.size() : size_t { 0 };

    for (size_t i = 0; i < points.size(); ++i)
    {
        auto point = points[i];
        point.key = i < numHeld ? held->points[i].key : givePointKey();
        points.set (i, point);
    }

    return { points.persistent() };
}

bool SongValues::setCurve (Song& song, const ParameterRef& parameter, CurveValue curve)
{
    // A curve alike to the one held (keys aside) is no change: the one held kept, its keys with it.
    const auto before = curves.get();
    const auto* held = before.find (parameter);

    if (held != nullptr && curve == *held)
        return false;

    setCurves (song, withCurve (before, parameter, keyByPlace (held, std::move (curve))));
    return ! curves.get().isSameAs (before);
}

bool SongValues::eraseCurve (Song& song, const ParameterRef& parameter)
{
    const auto before = curves.get();

    if (before.find (parameter) == nullptr)
        return false;

    setCurves (song, withoutCurve (before, parameter));
    return true;
}

void SongValues::renameCurve (Song& song, const ParameterRef& from, const ParameterRef& to)
{
    const auto& held = curves.get();

    if (from == to || held.find (from) == nullptr)
        return;

    // A curve's new parameter has none of its own (as withCurveRenamed() asks).
    const auto taken = held.find (to) != nullptr;
    jassert (! taken);

    if (! taken)
        setCurves (song, withCurveRenamed (held, from, to));
}

bool SongValues::setCurveOrder (Song& song, const std::vector<ParameterRef>& first)
{
    const auto before = curves.get();
    auto next = withCurveOrder (before, first);

    if (next.isSameAs (before))
        return false;

    setCurves (song, std::move (next));
    return true;
}

//==============================================================================
bool leadsToCurves (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return parent.hasType (schema::PHAZITE) && child.hasType (schema::AUTOMATION);
}

void writeCurvesIn (juce::ValueTree& element, const SongState& kept)
{
    if (! element.hasType (schema::AUTOMATION) || kept.curves.order.empty())
        return;

    writeCurves (kept.curves, element);
}

bool holdSameCurve (const SongState& kept, const SongState& otherKept, const ParameterRef& parameter)
{
    const auto* curve = kept.curves.find (parameter);
    const auto* other = otherKept.curves.find (parameter);

    if ((curve == nullptr) != (other == nullptr))
        return false;

    return curve == nullptr || *curve == *other;
}

bool holdSameCurveOrder (const SongState& kept, const SongState& otherKept)
{
    return kept.curves.order.identity() == otherKept.curves.order.identity() || kept.curves.order == otherKept.curves.order;
}

bool holdSameCurves (const SongState& kept, const SongState& otherKept)
{
    return kept.curves.isSameAs (otherKept.curves) || kept.curves == otherKept.curves;
}
} // namespace phazite
