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
#include <optional>

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

    // Whether a parameter steps, each point holding until the next: a choice or a switch of one of Phazite's own devices, or
    // a plug-in's that says so (its range moves in steps).
    bool isDiscrete (const Song& song, const ParameterRef& parameter)
    {
        const auto description = describeParameter (song, parameter);

        if (! description.has_value())
            return false;

        if (description->info.has_value())
            return description->info->unit == devices::Unit::choice || description->info->unit == devices::Unit::toggle;

        return description->range.interval > 0.0f;
    }
} // namespace

AutomationWriter::AutomationWriter (Song& s, PlaybackSession& owner)
    : song (s),
      session (owner)
{
}

AutomationWriter::~AutomationWriter()
{
    Gesture::forget (this);
}

void AutomationWriter::playbackStarted()
{
    // In write mode, every automated parameter of the track, from its lane's value where playback starts: the values set
    // by hand that go there are a step of their own, which no change before them takes along as it is undone.
    std::optional<RecordingStep> step;

    for (const auto& curve : getCurveStates (song))
    {
        if (const auto ref = getCurveParameter (curve); locateParameter (song, ref).has_value())
        {
            if (getModeOf (song, ref) == AutomationMode::write && findPass (ref) == nullptr)
            {
                // Begun only for a punch-in that sets a value (as punchIn() does): beginning a step keeps an open trial
                // (Swap), which nothing else here changes.
                const auto* lane = session.getPlayer().findCurve (ref);
                const auto setByHand = getValueSetByHand (song, ref);
                const auto value = lane != nullptr ? lane->getValueAt (getHeard().beat)
                                                   : session.getPlayer().getPlayedValue (ref, false).value_or (setByHand);

                if (! step.has_value() && ! juce::exactlyEqual (value, setByHand))
                    step.emplace (song, "Record Automation");

                punchIn (ref, AutomationMode::write, false);
            }
        }
    }

    // A control still grabbed from before playback started: its pass as it is now is what Escape goes back to, and it writes
    // from here as one grabbed now would (touch and latch punching in).
    std::vector<ParameterRef> held;

    for (const auto& grab : grabs)
        held.push_back (grab.parameter);

    for (const auto& parameter : held)
    {
        rememberGrab (parameter);
        gestureBegan (parameter);
    }
}

void AutomationWriter::playbackStopped()
{
    if (passes.empty())
        return;

    // A step of its own; during a drag (Stop is a key that works while the mouse is down), once the drag has ended, so that
    // its Escape takes back the drag alone.
    const RecordingStep step (song, "Record Automation");

    for (auto& pass : std::exchange (passes, {}))
        punchOut (std::move (pass), false);
}

void AutomationWriter::playheadJumped (BeatPosition to)
{
    if (passes.empty() || ! session.isPlaying())
        return;

    // Each pass to where the song is heard, as if it punched out there (round the loop first, if it went round since its
    // last move); and on from where the playhead lands, with the value it had. Not taken for the loop going round, which
    // would write it on to the loop's end.
    std::optional<RecordingStep> step;

    if (! Gesture::isInProgress (song))
        step.emplace (song, "Record Automation");

    const auto heard = getHeard();

    for (auto& pass : passes)
    {
        goRound (pass, heard.round);
        auto points = std::exchange (pass.points, { { to, pass.points.back().value, 0.0f } });
        finishPart (pass, std::move (points), BeatPosition::fromBeats (heard.beat));
    }
}

engine::PlayheadStamps::Place AutomationWriter::getHeard() const
{
    return session.getEngine().getPlayheadPlaceBefore (session.getHeardDelay());
}

void AutomationWriter::goRound (Pass& pass, std::uint32_t round)
{
    const auto times = (std::int32_t) (round - pass.round); // counted round, as the engine's counts are

    if (times <= 0)
        return;

    pass.round = round;

    if (! isLoopOn (song))
        return;

    const auto loop = getLoopBeats (song);
    const auto last = pass.points.back().value;
    auto points = std::exchange (pass.points, { { loop.getStart(), last, 0.0f } });
    finishPart (pass, std::move (points), loop.getEnd());

    if (times > 1)
        finishPart (pass, { { loop.getStart(), last, 0.0f } }, loop.getEnd());
}

void AutomationWriter::gestureBegan (const ParameterRef& parameter)
{
    const auto mode = getModeOf (song, parameter);

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
    gestureEnded (parameter, true);
}

void AutomationWriter::gestureEnded (const ParameterRef& parameter, bool ownDrag)
{
    // The grab is over. What its pass wrote at the loop's end or a jump during its own drag joins the drag's step, which
    // is kept; others' parts wait for the drag to end.
    std::erase_if (grabs, [&parameter] (const Grab& grab) { return grab.parameter == parameter; });
    auto* pass = findPass (parameter);

    if (ownDrag && pass != nullptr && Gesture::isInProgress (song))
        writeWaiting (&parameter);

    // Touch and write punch out as the control is let go, a track in write going on in latch; latch writes on.
    if (pass == nullptr || ! pass->held)
        return;

    pass->held = false;

    if (pass->mode == AutomationMode::latch)
        return;

    auto ended = *pass;
    std::erase_if (passes, [&parameter] (const Pass& each) { return each.parameter == parameter; });
    const auto mode = ended.mode;
    punchOut (std::move (ended), ownDrag);

    if (const auto track = getParameterTrack (song, parameter); track.isValid() && mode == AutomationMode::write)
        setAutomationMode (song, track, AutomationMode::latch);
}

bool AutomationWriter::isWriting (const ParameterRef& parameter) const
{
    return std::any_of (passes.begin(), passes.end(), [&parameter] (const Pass& pass) { return pass.parameter == parameter; });
}

void AutomationWriter::parameterRenamed (const ParameterRef& from, const ParameterRef& to)
{
    if (auto* pass = findPass (from); pass != nullptr && from != to)
    {
        session.getPlayer().setWriting (from, false);
        session.getPlayer().setWriting (to, true);
        pass->parameter = to;
    }
}

AutomationWriter::Pass* AutomationWriter::findPass (const ParameterRef& parameter)
{
    const auto found = std::find_if (passes.begin(), passes.end(), [&parameter] (const Pass& pass) { return pass.parameter == parameter; });
    return found != passes.end() ? &*found : nullptr;
}

AutomationWriter::Pass* AutomationWriter::punchIn (const ParameterRef& parameter, AutomationMode mode, bool byMove)
{
    const auto description = describeParameter (song, parameter);

    if (! description.has_value())
        return nullptr;

    // From the value the engine played there: its lane's, or the one set by hand before (for a pass a move starts, which
    // has set another). Grabbed, or punched in as playback starts, it goes on playing from there, set by hand.
    const auto heard = getHeard();
    const auto beat = BeatPosition::fromBeats (heard.beat);
    const auto* lane = session.getPlayer().findCurve (parameter);
    const auto setByHand = getValueSetByHand (song, parameter);
    const auto value =
        lane != nullptr ? lane->getValueAt (beat.inBeats()) : session.getPlayer().getPlayedValue (parameter, false).value_or (setByHand);

    session.getPlayer().setWriting (parameter, true);
    passes.push_back ({ parameter,
                        mode,
                        false,
                        description->range.getRange().getLength(),
                        isDiscrete (song, parameter),
                        { { beat, value, 0.0f } },
                        heard.round });

    if (! byMove && ! juce::exactlyEqual (value, setByHand))
    {
        const juce::ScopedValueSetter<bool> setting (settingValue, true);
        setValueSetByHand (song, parameter, value);
    }

    return &passes.back();
}

void AutomationWriter::record (Pass& pass, float value)
{
    // Back at the loop's start, as the engine counts its rounds: the pass so far goes to the loop's end, and another starts.
    // Heard a little earlier than the last move in the same round (the playhead heard estimated ahead as a block came late),
    // it is where that one was.
    const auto heard = getHeard();
    goRound (pass, heard.round);
    const auto beat = std::max (BeatPosition::fromBeats (heard.beat), pass.points.back().beat);
    pass.points.push_back ({ beat, value, pass.discrete ? 1.0f : 0.0f });
}

void AutomationWriter::punchOut (Pass pass, bool intoDrag)
{
    session.getPlayer().setWriting (pass.parameter, false);

    // Stopped after the loop went back to its start: to its end, then from its start.
    const auto heard = getHeard();
    goRound (pass, heard.round);
    auto points = std::move (pass.points);
    finishPart (pass, std::move (points), BeatPosition::fromBeats (heard.beat), intoDrag, ! intoDrag);
}

void AutomationWriter::finishPart (const Pass& pass, std::vector<AutomationPoint> points, BeatPosition end, bool intoDrag, bool ownStep)
{
    // During a drag, its step would hold it, and Escape take it back with the drag's moves: it waits; written into the drag
    // only if it is its own parameter's (and not a stop's), once the drag has ended otherwise.
    auto part = pass;
    part.points = std::move (points);
    waiting.push_back ({ std::move (part), end, ownStep });

    // Finished by an undo or a redo (followValuesSetByHand()), during which nothing may write through the song's history
    // (JUCE's UndoManager refuses it): once it has returned.
    if (song.getUndoManager().isPerformingUndoRedo())
        triggerAsyncUpdate();
    else if (! Gesture::isInProgress (song))
        writeWaiting();
    else if (intoDrag)
        writeWaiting (&pass.parameter);
    else
        writeWaitingAfterDrag();
}

void AutomationWriter::writeWaiting (const ParameterRef* only)
{
    // In order: each before what its parameter wrote later.
    std::vector<Part> written;

    for (auto it = waiting.begin(); it != waiting.end();)
    {
        if (only == nullptr || (it->pass.parameter == *only && ! it->ownStep))
        {
            written.push_back (std::move (*it));
            it = waiting.erase (it);
        }
        else
        {
            ++it;
        }
    }

    for (auto& part : written)
        write (part.pass, std::move (part.pass.points), part.end);
}

void AutomationWriter::writeWaitingAfterDrag()
{
    if (std::exchange (writeAsked, true))
        return;

    Gesture::whenEnded (song, this,
                        [this]
                        {
                            writeAsked = false;

                            if (waiting.empty())
                                return;

                            const RecordingStep step (song, "Record Automation");
                            writeWaiting();
                        });
}

void AutomationWriter::handleAsyncUpdate()
{
    if (waiting.empty()) // written meanwhile, as playback stopped or the playhead jumped
        return;

    if (Gesture::isInProgress (song))
    {
        writeWaitingAfterDrag();
        return;
    }

    const RecordingStep step (song, "Record Automation");
    writeWaiting();
}

void AutomationWriter::write (const Pass& pass, std::vector<AutomationPoint> points, BeatPosition end)
{
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
    if (! session.isPlayingProject() || ! locateParameter (song, parameter).has_value())
        return;

    if (starting)
    {
        gestureBegan (parameter);
        return;
    }

    // Not a step of the control's drag under way, if there is one: its Escape would take it back.
    const RecordingStep step (song, "Record Automation");
    gestureEnded (parameter, false);
}

void AutomationWriter::parameterChanged (const ParameterRef& parameter, float valueSetByHand)
{
    // A control moved by hand: written if its pass is on, or if its track is in latch or write (touch needs it grabbed). What
    // sets many values at once (a preset) begins no pass, but one that runs writes what it set, as the engine plays it.
    if (settingValue || ! session.isPlaying() || ! parameter.isValid())
        return;

    auto* pass = findPass (parameter);

    if (pass == nullptr && settingManyValues == 0)
        if (const auto mode = getModeOf (song, parameter); mode == AutomationMode::latch || mode == AutomationMode::write)
            pass = punchIn (parameter, mode, true);

    if (pass != nullptr)
        record (*pass, valueSetByHand);
}

void AutomationWriter::rememberGrab (const ParameterRef& parameter)
{
    std::erase_if (grabs, [&parameter] (const Grab& grab) { return grab.parameter == parameter; });
    const auto* pass = findPass (parameter);
    grabs.push_back (
        { parameter, pass != nullptr, pass != nullptr ? pass->points.size() : 0, pass != nullptr ? pass->points.back().value : 0.0f });
}

void AutomationWriter::gestureCancelled (const ParameterRef& parameter)
{
    const auto found = std::find_if (grabs.begin(), grabs.end(), [&parameter] (const Grab& grab) { return grab.parameter == parameter; });

    if (found == grabs.end())
        return;

    const auto grab = *found;
    grabs.erase (found);
    auto* pass = findPass (parameter);

    // What its pass wrote at the loop's end or a jump since the grab, which waited for the drag's end: as before the grab.
    // Begun by the grab, none of it; otherwise its first part as far as the grab, held on to the part's end, and each part
    // after it the value from before the grab, from its start to its end. The rest, others' parts, in a step of their own.
    auto first = true;

    std::erase_if (waiting,
                   [&] (Part& part)
                   {
                       if (part.pass.parameter != parameter)
                           return false;

                       if (! grab.hadPass)
                           return true;

                       auto& points = part.pass.points;

                       if (std::exchange (first, false))
                           points.resize (std::clamp<size_t> (grab.points, 1, points.size()));
                       else
                           points = { { points.front().beat, grab.value, 0.0f } };

                       part.ownStep = true;
                       return false;
                   });

    // Called just before the drag's step is rolled back, whose step would take them back: once it has been.
    if (! waiting.empty())
    {
        if (Gesture::isInProgress (song))
        {
            writeWaitingAfterDrag();
        }
        else
        {
            const RecordingStep step (song, "Record Automation");
            writeWaiting();
        }
    }

    // The rollback takes the values set by hand back to where they were as the drag began, punch-ins of the passes made
    // during it too (Play pressed with the control held, in Write): each pass's value set by hand is put back to what it
    // writes, so that what is heard is what is written (known bug 174).
    Gesture::whenEnded (song, this, [this] { keepPassesValues(); });

    if (pass == nullptr)
        return;

    // A pass the grab began (touch, or latch until then): gone, as if the control was never grabbed.
    if (! grab.hadPass)
    {
        session.getPlayer().setWriting (parameter, false);
        std::erase_if (passes, [&parameter] (const Pass& each) { return each.parameter == parameter; });
        return;
    }

    // One that wrote before (latch, write): the moves since go, and it goes on from where it was; gone round the loop or
    // jumped since the grab, from where this part started, with the value from before the grab.
    if (! first)
        pass->points = { { pass->points.front().beat, grab.value, 0.0f } };
    else if (pass->points.size() > grab.points)
        pass->points.resize (std::max<size_t> (1, grab.points));

    pass->held = false;
}

void AutomationWriter::followValuesSetByHand()
{
    // An undo or a redo while a pass writes, which set a value by hand another way than a control does (which parameterChanged()
    // hears): what plays from then is what the pass writes. Not a drag's rollback, after which keepPassesValues() sets what
    // the pass writes.
    if (settingValue || Gesture::isInProgress (song))
        return;

    for (auto& pass : passes)
        if (const auto value = getValueSetByHand (song, pass.parameter);
            ! pass.points.empty() && ! juce::exactlyEqual (value, pass.points.back().value))
            record (pass, value);
}

void AutomationWriter::keepPassesValues()
{
    // As a punch-in sets it (punchIn()), in a step of its own: Escape took back the drag alone. An Undo of it while the pass
    // goes on sets the value back, which the pass then writes (followValuesSetByHand()): what plays is what is written.
    std::optional<RecordingStep> step;

    for (const auto& pass : passes)
    {
        if (pass.points.empty() || ! locateParameter (song, pass.parameter).has_value())
            continue;

        if (const auto writes = pass.points.back().value; ! juce::exactlyEqual (getValueSetByHand (song, pass.parameter), writes))
        {
            if (! step.has_value())
                step.emplace (song, "Record Automation");

            const juce::ScopedValueSetter<bool> setting (settingValue, true);
            setValueSetByHand (song, pass.parameter, writes);
        }
    }
}
} // namespace phazite
