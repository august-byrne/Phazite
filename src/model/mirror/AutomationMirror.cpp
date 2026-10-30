// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Mirror.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"

namespace phazite::mirror
{
namespace
{
    // A point as Tracktion's curve keeps it: in seconds (a device's parameter's curve is in time), its value and its curve.
    struct TracktionPoint
    {
        double seconds = 0.0;
        float value = 0.0f, curve = 0.0f;

        bool isIn (const juce::ValueTree& point) const
        {
            return juce::exactlyEqual ((double) point[te::IDs::t], seconds) && juce::exactlyEqual ((float) point[te::IDs::v], value)
                   && juce::exactlyEqual ((float) point[te::IDs::c], curve);
        }
    };

    std::vector<TracktionPoint> getTracktionPoints (const te::TempoSequence& tempo, const juce::ValueTree& curveState)
    {
        std::vector<TracktionPoint> points;

        for (const auto& point : curveState)
            points.push_back ({ tempo.toTime (te::BeatPosition::fromBeats ((double) point[schema::pointBeat])).inSeconds(),
                                point[schema::pointValue], point.getProperty (schema::pointCurve, 0.0f) });

        return points;
    }

    // Tracktion's curve with the song's points, in the song's order: those that match at its start and its end stay, as
    // an edit changes a point or a few in a row; between them, Tracktion's points are set to the song's, those too many go
    // and those missing are added where they go.
    void reconcileCurveOf (te::AutomatableParameter& parameter, const juce::ValueTree& curveState)
    {
        // Tracktion's while it records it, until the song takes in what it recorded (adoptRecordedAutomation()).
        if (parameter.isCurrentlyRecording())
            return;

        auto& curve = parameter.getCurve();
        auto* undo = &parameter.getEdit().getUndoManager();
        const auto wanted = getTracktionPoints (parameter.getEdit().tempoSequence, curveState);
        const auto want = (int) wanted.size();
        const auto have = curve.getNumPoints();

        auto first = 0;

        while (first < have && first < want && wanted[(size_t) first].isIn (curve.state.getChild (first)))
            ++first;

        auto last = 0;

        while (last < have - first && last < want - first
               && wanted[(size_t) (want - 1 - last)].isIn (curve.state.getChild (have - 1 - last)))
            ++last;

        const auto haveBetween = have - first - last, wantBetween = want - first - last;

        for (auto i = wantBetween; i < haveBetween; ++i)
            curve.removePoint (first + wantBetween, undo); // empty, the curve leaves the device's state

        for (auto i = 0; i < std::min (haveBetween, wantBetween); ++i)
        {
            const auto& point = wanted[(size_t) (first + i)];
            auto state = curve.state.getChild (first + i);

            if (! point.isIn (state))
            {
                state.setProperty (te::IDs::t, point.seconds, undo);
                state.setProperty (te::IDs::v, point.value, undo);
                state.setProperty (te::IDs::c, point.curve, undo);
            }
        }

        for (auto i = haveBetween; i < wantBetween; ++i)
        {
            const auto& point = wanted[(size_t) (first + i)];
            const auto time = te::TimePosition::fromSeconds (point.seconds);

            // The first through the curve, which puts its state into the device's; the others where they go among those.
            if (curve.getNumPoints() == 0)
                curve.addPoint (time, point.value, point.curve, undo);
            else
                curve.state.addChild (te::AutomationCurve::AutomationPoint (time, point.value, point.curve).toValueTree(), first + i, undo);
        }
    }
} // namespace

void reconcileCurve (te::Edit& edit, const ParameterRef& parameter)
{
    if (const auto found = findParameter (edit, parameter))
        reconcileCurveOf (*found, findCurveState (edit, parameter));
}

void reconcileAutomation (te::Edit& edit)
{
    for (const auto& curveState : getCurveStates (edit))
        if (const auto parameter = findParameter (edit, getCurveParameter (curveState)))
            reconcileCurveOf (*parameter, curveState);

    // Tracktion's curves that the song hasn't.
    for (auto* device : te::getAllPlugins (edit, true))
        for (auto* parameter : device->getAutomatableParameters())
            if (parameter->getCurve().getNumPoints() > 0 && ! findCurveState (edit, getParameterRef (*parameter)).isValid())
                reconcileCurveOf (*parameter, {});
}
} // namespace phazite::mirror
