// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/AutomationWriter.h"
#include "internal/PlaybackSession.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Parameters.h"
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

    AutomationMode getModeOf (const Song& song, const ParameterRef& parameter)
    {
        const auto track = getParameterTrack (song, parameter);
        return track.isValid() ? getAutomationMode (song, track) : AutomationMode::read;
    }

    // Whether a parameter steps, each point holding until the next: a choice or a switch of one of Phazite's own devices,
    // or a plug-in's that says so (Tracktion's parameter, until the session owns the plug-ins' instances: T6 step 5).
    bool isDiscrete (const Song& song, const ParameterRef& parameter)
    {
        if (const auto description = describeParameter (song, parameter); description.has_value() && description->info.has_value())
            return description->info->unit == devices::Unit::choice || description->info->unit == devices::Unit::toggle;

        auto* edit = findEdit (song);
        const auto tracktions = edit != nullptr ? findParameter (*edit, parameter) : nullptr;
        return tracktions != nullptr && tracktions->isDiscrete();
    }
} // namespace

AutomationWriter::AutomationWriter (te::Edit& e, PlaybackSession& s)
    : edit (e),
      session (s)
{
}

AutomationWriter::~AutomationWriter()
{
    if (! session.hasEdit())
        return;

    for (const auto& pass : passes)
        if (const auto parameter = findParameter (edit, pass.parameter))
            parameter->resetRecordingStatus();
}

void AutomationWriter::playbackStarted()
{
    // In write mode, every automated parameter of the track, from its lane's value where playback starts.
    auto& song = getSong (edit);

    for (const auto& curve : getCurveStates (song))
        if (const auto ref = getCurveParameter (curve); locateParameter (song, ref).has_value())
            if (getModeOf (song, ref) == AutomationMode::write && findPass (ref) == nullptr)
                punchIn (ref, AutomationMode::write, false);
}

void AutomationWriter::playbackStopped()
{
    if (passes.empty())
        return;

    const RecordingStep step (getSong (edit), "Record Automation");

    for (const auto& pass : std::exchange (passes, {}))
        punchOut (pass);
}

void AutomationWriter::gestureBegan (const ParameterRef& parameter)
{
    const auto mode = getModeOf (getSong (edit), parameter);

    if (! session.isPlaying() || mode == AutomationMode::read)
        return;

    auto* pass = findPass (parameter);

    if (pass == nullptr)
        pass = punchIn (parameter, mode, false);

    if (pass != nullptr)
        pass->held = true;
}

void AutomationWriter::gestureEnded (const ParameterRef& parameter)
{
    // Touch and write punch out as the control is let go, a track in write going on in latch; latch writes on.
    auto* pass = findPass (parameter);

    if (pass == nullptr || ! pass->held)
        return;

    pass->held = false;

    if (pass->mode == AutomationMode::latch)
        return;

    const auto ended = *pass;
    std::erase_if (passes, [&parameter] (const Pass& each) { return each.parameter == parameter; });
    punchOut (ended);

    auto& song = getSong (edit);

    if (const auto track = getParameterTrack (song, parameter); track.isValid() && ended.mode == AutomationMode::write)
        setAutomationMode (song, track, AutomationMode::latch);
}

bool AutomationWriter::isWriting (const ParameterRef& parameter) const
{
    return std::any_of (passes.begin(), passes.end(), [&parameter] (const Pass& pass) { return pass.parameter == parameter; });
}

AutomationWriter::Pass* AutomationWriter::findPass (const ParameterRef& parameter)
{
    const auto found = std::find_if (passes.begin(), passes.end(), [&parameter] (const Pass& pass) { return pass.parameter == parameter; });
    return found != passes.end() ? &*found : nullptr;
}

AutomationWriter::Pass* AutomationWriter::punchIn (const ParameterRef& parameter, AutomationMode mode, bool byMove)
{
    auto& song = getSong (edit);
    const auto description = describeParameter (song, parameter);

    if (! description.has_value())
        return nullptr;

    // From the value the engine played there: its lane's, or the one set by hand before (for a pass a move starts, which
    // has set another). Grabbed, or punched in as playback starts, it goes on playing from there, set by hand.
    const auto beat = session.getPlayhead();
    const auto* lane = session.getPlayer().findCurve (parameter);
    const auto setByHand = getValueSetByHand (song, parameter);
    const auto value =
        lane != nullptr ? lane->getValueAt (beat.inBeats()) : session.getPlayer().getPlayedValue (parameter, false).value_or (setByHand);

    session.getPlayer().setWriting (parameter, true);
    passes.push_back (
        { parameter, mode, false, description->range.getRange().getLength(), isDiscrete (song, parameter), { { beat, value, 0.0f } } });

    if (! byMove && ! juce::exactlyEqual (value, setByHand))
    {
        const juce::ScopedValueSetter<bool> setting (settingValue, true);
        setValueSetByHand (song, parameter, value);
    }

    // Tracktion's parameter, which a plug-in's own window moves: its curve's one point stays meanwhile (adoptMovedPoint()).
    if (const auto tracktions = findParameter (edit, parameter))
        tracktions->startRecordingStatus();

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
    auto& song = getSong (edit);

    if (! locateParameter (song, pass.parameter).has_value()) // its device gone meanwhile
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

    replacePoints (song, pass.parameter, BeatRange (start, end), points);
}

void AutomationWriter::pluginGestureChanged (const ParameterRef& parameter, bool starting)
{
    if (! session.isPlayingProject() || ! locateParameter (getSong (edit), parameter).has_value())
        return;

    if (starting)
    {
        gestureBegan (parameter);
        return;
    }

    const RecordingStep step (getSong (edit), "Record Automation");
    gestureEnded (parameter);
}

void AutomationWriter::parameterChanged (const ParameterRef& parameter, float valueSetByHand, bool isFollowingAutomation)
{
    // A control moved by hand: written if its pass is on, or if its track is in latch or write (touch needs it grabbed).
    if (isFollowingAutomation || settingValue || ! session.isPlaying() || ! session.hasEdit() || ! parameter.isValid())
        return;

    auto* pass = findPass (parameter);

    if (pass == nullptr)
        if (const auto mode = getModeOf (getSong (edit), parameter); mode == AutomationMode::latch || mode == AutomationMode::write)
            pass = punchIn (parameter, mode, true);

    if (pass != nullptr)
        record (*pass, valueSetByHand);
}
} // namespace phazite
