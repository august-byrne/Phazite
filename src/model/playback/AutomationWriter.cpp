// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/AutomationWriter.h"
#include "internal/PlaybackSession.h"
#include "model/automation/internal/Automation.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/internal/Mixer.h"
#include "model/transport/internal/Playback.h"

#include <algorithm>
#include <cmath>

namespace phazite
{
namespace
{
    constexpr double thinning = 0.002; // a move within this much of the line, as a part of the parameter's range, is left out

    // The moves that change the line: each further from the line between the last one kept and the next than the thinning.
    std::vector<AutomationPoint> thin (const std::vector<AutomationPoint>& points, float range)
    {
        if (points.size() < 3)
            return points;

        std::vector<AutomationPoint> kept { points.front() };

        for (size_t i = 1; i + 1 < points.size(); ++i)
        {
            const auto& from = kept.back();
            const auto& to = points[i + 1];
            const auto span = (to.beat - from.beat).inBeats();
            const auto along = span > 0.0 ? (float) ((points[i].beat - from.beat).inBeats() / span) : 0.0f;

            if (std::abs (from.value + along * (to.value - from.value) - points[i].value) > (float) thinning * range)
                kept.push_back (points[i]);
        }

        kept.push_back (points.back());
        return kept;
    }

    AutomationMode getModeOf (const te::AutomatableParameter& parameter)
    {
        const auto* track = getTrackOfParameter (parameter);
        return track != nullptr ? getAutomationMode (*track) : AutomationMode::read;
    }
} // namespace

AutomationWriter::AutomationWriter (te::Edit& e, PlaybackSession& s)
    : edit (e),
      exists (&e),
      session (s)
{
    edit.getParameterChangeHandler().addListener (*this);
}

AutomationWriter::~AutomationWriter()
{
    if (exists.get() == nullptr)
        return;

    for (const auto& pass : passes)
        if (const auto parameter = findParameter (edit, pass.parameter))
            parameter->resetRecordingStatus();

    edit.getParameterChangeHandler().removeListener (*this);
}

void AutomationWriter::playbackStarted()
{
    // In write mode, every automated parameter of the track, from its lane's value where playback starts.
    for (const auto& curve : getCurveStates (edit))
        if (const auto parameter = findParameter (edit, getCurveParameter (curve)))
            if (getModeOf (*parameter) == AutomationMode::write && findPass (getParameterRef (*parameter)) == nullptr)
                punchIn (*parameter, AutomationMode::write, false);
}

void AutomationWriter::playbackStopped()
{
    if (passes.empty())
        return;

    const RecordingStep step (edit, "Record Automation");

    for (const auto& pass : std::exchange (passes, {}))
        punchOut (pass);
}

void AutomationWriter::gestureBegan (te::AutomatableParameter& parameter)
{
    const auto mode = getModeOf (parameter);

    if (! session.isPlaying() || mode == AutomationMode::read)
        return;

    auto* pass = findPass (getParameterRef (parameter));

    if (pass == nullptr)
        pass = punchIn (parameter, mode, false);

    if (pass != nullptr)
        pass->held = true;
}

void AutomationWriter::gestureEnded (te::AutomatableParameter& parameter)
{
    // Touch and write punch out as the control is let go, a track in write going on in latch; latch writes on.
    const auto ref = getParameterRef (parameter);
    auto* pass = findPass (ref);

    if (pass == nullptr || ! pass->held)
        return;

    pass->held = false;

    if (pass->mode == AutomationMode::latch)
        return;

    const auto ended = *pass;
    std::erase_if (passes, [&ref] (const Pass& each) { return each.parameter == ref; });
    punchOut (ended);

    if (auto* track = getTrackOfParameter (parameter); track != nullptr && ended.mode == AutomationMode::write)
        setAutomationMode (*track, AutomationMode::latch);
}

AutomationWriter::Pass* AutomationWriter::findPass (const ParameterRef& parameter)
{
    const auto found = std::find_if (passes.begin(), passes.end(), [&parameter] (const Pass& pass) { return pass.parameter == parameter; });
    return found != passes.end() ? &*found : nullptr;
}

AutomationWriter::Pass* AutomationWriter::punchIn (te::AutomatableParameter& parameter, AutomationMode mode, bool byMove)
{
    const auto ref = getParameterRef (parameter);

    if (! ref.isValid())
        return nullptr;

    // From the value the engine played there: its lane's, or the one set by hand before (for a pass a move starts, which
    // has set another). Grabbed, or punched in as playback starts, it goes on playing from there, set by hand.
    const auto beat = session.getPlayhead();
    const auto* lane = session.getPlayer().findCurve (ref);
    const auto value = lane != nullptr ? lane->getValueAt (beat.inBeats())
                                       : session.getPlayer().getPlayedValue (ref, false).value_or (parameter.getCurrentBaseValue());

    if (! byMove && ! juce::exactlyEqual (value, parameter.getCurrentBaseValue()))
    {
        const juce::ScopedValueSetter<bool> setting (settingValue, true);
        setDeviceValue (parameter, value);
    }

    session.getPlayer().setWriting (ref, true);
    parameter.startRecordingStatus(); // so that a curve of one point isn't moved to the control meanwhile (adoptMovedPoint())
    passes.push_back ({ ref, mode, false, parameter.getValueRange().getLength(), parameter.isDiscrete(), { { beat, value, 0.0f } } });
    return &passes.back();
}

void AutomationWriter::record (Pass& pass, float value)
{
    // Back at the loop's start: the pass so far goes to the loop's end, and another starts.
    const auto beat = session.getPlayhead();

    if (beat < pass.points.back().beat && isLoopOn (edit))
    {
        const auto loop = getLoopBeats (edit);
        const auto last = pass.points.back().value;
        write (pass, std::exchange (pass.points, { { loop.getStart(), last, 0.0f } }), loop.getEnd());
    }

    pass.points.push_back ({ beat, value, pass.discrete ? 1.0f : 0.0f });
}

void AutomationWriter::punchOut (const Pass& pass)
{
    session.getPlayer().setWriting (pass.parameter, false);

    if (const auto parameter = findParameter (edit, pass.parameter))
        parameter->resetRecordingStatus();

    // Stopped after the loop went back to its start: to its end, then from its start.
    const auto end = session.getPlayhead();
    auto points = pass.points;

    if (end < points.back().beat && isLoopOn (edit))
    {
        const auto loop = getLoopBeats (edit);
        const auto last = points.back().value;
        write (pass, std::exchange (points, { { loop.getStart(), last, 0.0f } }), loop.getEnd());
    }

    write (pass, std::move (points), end);
}

void AutomationWriter::write (const Pass& pass, std::vector<AutomationPoint> points, BeatPosition end)
{
    const auto parameter = findParameter (edit, pass.parameter);

    if (parameter == nullptr) // its device gone meanwhile
        return;

    // The moves that change the line, held to the end, where the lane's value comes back (the lane as it was before).
    const auto* lane = session.getPlayer().findCurve (pass.parameter);
    const auto start = points.front().beat;
    end = std::max (end, points.back().beat);
    points = thin (points, pass.range);
    points.push_back ({ end, points.back().value, 0.0f });

    if (lane != nullptr)
        if (const auto laneValue = lane->getValueAt (end.inBeats()); ! juce::exactlyEqual (laneValue, points.back().value))
            points.push_back ({ end, laneValue, 0.0f });

    replacePoints (*parameter, BeatRange (start, end), points);
}

void AutomationWriter::pluginParameterChanged (te::AutomatableParameter& parameter, bool isFollowingAutomation)
{
    // A control moved by hand: written if its pass is on, or if its track is in latch or write (touch needs it grabbed).
    if (isFollowingAutomation || settingValue || ! session.isPlaying() || exists.get() == nullptr)
        return;

    auto* pass = findPass (getParameterRef (parameter));

    if (pass == nullptr)
        if (const auto mode = getModeOf (parameter); mode == AutomationMode::latch || mode == AutomationMode::write)
            pass = punchIn (parameter, mode, true);

    if (pass != nullptr)
        record (*pass, parameter.getCurrentBaseValue());
}
} // namespace phazite
