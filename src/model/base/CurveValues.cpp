// SPDX-License-Identifier: AGPL-3.0-or-later
#include "model/base/internal/CurveValues.h"
#include "model/base/Numbers.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace phazite
{
namespace
{
    // A point's bend where its element has none: a straight line.
    constexpr float straight = 0.0f;

    std::atomic<std::uint64_t> pointsRead { 0 }; // countCurvePointsRead(): a count alone, which orders nothing

} // namespace

bool PointValue::operator== (const PointValue& other) const noexcept
{
    return juce::exactlyEqual (beat.inBeats(), other.beat.inBeats()) && juce::exactlyEqual (position, other.position)
           && juce::exactlyEqual (curve, other.curve);
}

bool CurveValue::operator== (const CurveValue& other) const
{
    if (isSameAs (other))
        return true;

    if (points.size() != other.points.size())
        return false;

    pointsRead.fetch_add (points.size(), std::memory_order_relaxed);
    return points == other.points;
}

std::size_t ParameterRefHash::operator() (const ParameterRef& parameter) const noexcept
{
    const auto device = parameter.device.getRaw();
    return (std::size_t) (device ^ (device >> 32)) ^ (std::size_t) parameter.parameter.hash();
}

bool SongCurves::isSameAs (const SongCurves& other) const noexcept
{
    return curves.identity() == other.curves.identity() && order.identity() == other.order.identity();
}

bool SongCurves::operator== (const SongCurves& other) const
{
    if (isSameAs (other))
        return true;

    if (order != other.order)
        return false;

    for (const auto& parameter : order)
    {
        const auto* mine = find (parameter);
        const auto* theirs = other.find (parameter);

        if (mine == nullptr || theirs == nullptr || ! (*mine == *theirs))
            return false;
    }

    return true;
}

SongCurves withCurve (const SongCurves& curves, const ParameterRef& parameter, CurveValue curve)
{
    auto changed = curves;

    if (curves.find (parameter) == nullptr)
        changed.order = changed.order.push_back (parameter);

    changed.curves = changed.curves.set (parameter, std::move (curve));
    return changed;
}

SongCurves withoutCurve (const SongCurves& curves, const ParameterRef& parameter)
{
    if (curves.find (parameter) == nullptr)
        return curves;

    auto changed = curves;
    changed.curves = changed.curves.erase (parameter);

    for (size_t i = 0; i < changed.order.size(); ++i)
    {
        if (changed.order[i] == parameter)
        {
            changed.order = changed.order.erase (i);
            break;
        }
    }

    return changed;
}

SongCurves withCurveRenamed (const SongCurves& curves, const ParameterRef& from, const ParameterRef& to)
{
    const auto* curve = curves.find (from);

    if (curve == nullptr || from == to)
        return curves;

    jassert (curves.find (to) == nullptr); // a curve's new parameter has none of its own
    const auto moved = *curve;
    auto changed = curves;
    changed.curves = changed.curves.erase (from).set (to, moved);

    for (size_t i = 0; i < changed.order.size(); ++i)
    {
        if (changed.order[i] == from)
        {
            changed.order = changed.order.set (i, to);
            break;
        }
    }

    return changed;
}

SongCurves withCurveOrder (const SongCurves& curves, const std::vector<ParameterRef>& first)
{
    std::vector<ParameterRef> order;
    order.reserve (curves.order.size());

    for (const auto& parameter : first)
        if (curves.find (parameter) != nullptr && std::find (order.begin(), order.end(), parameter) == order.end())
            order.push_back (parameter);

    for (const auto& parameter : curves.order)
        if (std::find (order.begin(), order.end(), parameter) == order.end())
            order.push_back (parameter);

    if (std::equal (order.begin(), order.end(), curves.order.begin(), curves.order.end()))
        return curves;

    auto changed = curves;
    changed.order = immer::flex_vector<ParameterRef> (order.begin(), order.end());
    return changed;
}

bool holdsItsOrder (const SongCurves& curves)
{
    if (curves.order.size() != curves.curves.size())
        return false;

    std::vector<ParameterRef> seen;
    seen.reserve (curves.order.size());

    for (const auto& parameter : curves.order)
    {
        if (curves.find (parameter) == nullptr || std::find (seen.begin(), seen.end(), parameter) != seen.end())
            return false;

        seen.push_back (parameter);
    }

    return true;
}

juce::String getCurveKeyText (const ParameterRef& parameter)
{
    return juce::String ((juce::int64) parameter.device.getRaw()) + "/" + parameter.parameter;
}

std::optional<ParameterRef> readCurveKeyText (const juce::String& text)
{
    const auto split = text.indexOfChar ('/');

    if (split < 0)
        return std::nullopt;

    return ParameterRef { DeviceID::fromRaw (readID (text.substring (0, split))), text.substring (split + 1) };
}

//==============================================================================
ParameterRef readCurveParameter (const juce::ValueTree& curve)
{
    return { DeviceID::fromRaw (readID (curve[schema::curveDevice])), curve[schema::curveParameter].toString() };
}

CurveValue readHeldCurve (const juce::ValueTree& curve, std::uint64_t* nextKey)
{
    auto points = immer::flex_vector<PointValue>().transient();

    for (const auto& child : curve)
    {
        if (! child.hasType (schema::POINT))
            continue;

        // A value that is no finite number: no point (the loader takes one out as the song opens).
        if (const auto value = readValueNumber (child[schema::pointValue]); ! value.has_value() || ! std::isfinite (*value))
            continue;

        points.push_back ({ BeatPosition::fromBeats (readDescribed (child, schema::pointBeat)),
                            toFloat (readDescribed (child, schema::pointValue)), toFloat (readDescribed (child, schema::pointCurve)),
                            nextKey != nullptr ? (*nextKey)++ : std::uint64_t { 0 } });
    }

    pointsRead.fetch_add (points.size(), std::memory_order_relaxed);
    return { points.persistent() };
}

SongCurves readHeldCurves (const juce::ValueTree& automation, std::uint64_t& nextKey)
{
    SongCurves read;
    auto curves = read.curves.transient();
    auto order = read.order.transient();

    for (const auto& child : automation)
    {
        if (! child.hasType (schema::CURVE))
            continue;

        if (const auto parameter = readCurveParameter (child); curves.find (parameter) == nullptr)
        {
            curves.set (parameter, readHeldCurve (child, &nextKey));
            order.push_back (parameter);
        }
    }

    read.curves = curves.persistent();
    read.order = order.persistent();
    return read;
}

std::uint64_t countCurvePointsRead() noexcept
{
    return pointsRead.load (std::memory_order_relaxed);
}

void writeCurveElement (const ParameterRef& parameter, const CurveValue& curve, juce::ValueTree& automation)
{
    jassert (automation.hasType (schema::AUTOMATION));
    juce::ValueTree element (schema::CURVE, { { schema::curveDevice, juce::String ((juce::int64) parameter.device.getRaw()) },
                                              { schema::curveParameter, parameter.parameter } });

    immer::for_each (curve.points,
                     [&element] (const PointValue& point)
                     {
                         juce::ValueTree written (schema::POINT,
                                                  { { schema::pointBeat, point.beat.inBeats() }, { schema::pointValue, point.position } });

                         if (! juce::exactlyEqual (point.curve, straight))
                             written.setProperty (schema::pointCurve, point.curve, nullptr);

                         element.appendChild (written, nullptr);
                     });

    automation.appendChild (element, nullptr);
}

void writeCurves (const SongCurves& curves, juce::ValueTree& automation)
{
    for (const auto& parameter : curves.order)
        if (const auto* curve = curves.find (parameter))
            writeCurveElement (parameter, *curve, automation);
}
} // namespace phazite
