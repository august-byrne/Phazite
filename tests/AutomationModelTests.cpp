// SPDX-License-Identifier: AGPL-3.0-or-later
// Automation by IDs and plain values (src/model/automation/AutomationModel.h): the parameters a track offers, the lanes it
// shows as view state, points and their edits as named undo steps, shapes, automation that keeps its beats when the
// tempo changes, the write modes and what touch records, the selected points' commands, and the parameter touched last.
#include "GoldenFiles.h"
#include "RealTimePlayback.h"
#include "TestHelpers.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/SongSelection.h"
#include "model/arrangement/internal/Selection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/internal/Automation.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <cmath>

namespace
{
using phazite::AutomationMode;
using phazite::AutomationShape;
using phazite::BeatDuration;
using phazite::ParameterKind;
using phazite::ParameterRef;

phazite::BeatPosition beats (double b)
{
    return phazite::BeatPosition::fromBeats (b);
}

phazite::TrackID idOf (const te::Track& track)
{
    return phazite::getTrackID (track);
}

// An open project with two tracks, A (with a synth, then a compressor) and B, as the app has it, and no undo history.
struct Song
{
    Song()
    {
        edit->ensureNumberOfAudioTracks (2);
        a = te::getAudioTracks (*edit)[0];
        b = te::getAudioTracks (*edit)[1];
        a->setName ("A");
        b->setName ("B");
        setEdit (context, edit.get());
        phazite::chooseInstrument (context, idOf (*a), find ("phaziteSynth"));
        phazite::addEffect (context, idOf (*a), find ("phaziteCompressor"));
        edit->getUndoManager().clearUndoHistory();
    }

    ~Song() { setEdit (context, nullptr); }

    phazite::DeviceType find (const char* type) const
    {
        for (const auto& each : phazite::getDeviceTypes (context))
            if (each.description.fileOrIdentifier == juce::String (type))
                return each;

        FAIL ("no such device");
        return {};
    }

    // A's compressor's threshold, from -60 to 0 dB, and A's volume.
    ParameterRef threshold() const
    {
        const auto effects = phazite::getEffects (context, idOf (*a));
        REQUIRE (effects.size() == 1);
        return { effects[0].id, "threshold" };
    }

    ParameterRef volume() { return phazite::getVolume (context, idOf (*a)).getRef(); }

    juce::String describeUndo() const { return phazite::describeUndo (edit.get()); }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    te::AudioTrack* a = nullptr;
    te::AudioTrack* b = nullptr;
};

std::vector<ParameterRef> lanesOf (const phazite::ProjectContext& context, phazite::TrackID track)
{
    std::vector<ParameterRef> parameters;

    for (const auto& lane : phazite::getAutomationLanes (context, track))
        parameters.push_back (lane.parameter);

    return parameters;
}

std::vector<double> beatsOf (const std::vector<phazite::AutomationPoint>& points)
{
    std::vector<double> result;

    for (const auto& point : points)
        result.push_back (point.beat.inBeats());

    return result;
}
} // namespace

TEST_CASE ("A track's automatable parameters are its volume and pan, then its devices', and the master has its own")
{
    Song song;
    auto& context = song.context;
    const auto targets = phazite::getAutomationTargets (context, idOf (*song.a));
    REQUIRE (targets.size() > 4);

    CHECK (targets[0].parameter == song.volume());
    CHECK (targets[0].name == juce::String ("Volume"));
    CHECK (targets[0].deviceName == juce::String ("Mixer"));
    CHECK (targets[0].kind == ParameterKind::volume);
    CHECK (targets[1].name == juce::String ("Pan"));
    CHECK (targets[1].kind == ParameterKind::pan);

    // The compressor's threshold among its devices', none of them a device's dry or wet level (its mix is one knob).
    const auto threshold =
        std::find_if (targets.begin(), targets.end(), [&song] (const auto& t) { return t.parameter == song.threshold(); });
    REQUIRE (threshold != targets.end());
    CHECK (threshold->name == juce::String ("Threshold"));
    CHECK (threshold->deviceName == juce::String ("Compressor"));
    CHECK (threshold->track == idOf (*song.a));
    CHECK (threshold->kind == ParameterKind::other);
    CHECK_FALSE (threshold->automated);
    CHECK (std::none_of (targets.begin(), targets.end(),
                         [] (const auto& t) { return t.name.containsIgnoreCase ("dry") || t.name.containsIgnoreCase ("wet"); }));

    // Each by its ref, and as a handle to change it.
    CHECK (phazite::getAutomationTarget (context, song.threshold())->name == juce::String ("Threshold"));
    CHECK (phazite::getParameter (context, song.threshold()).getName() == juce::String ("Threshold"));
    CHECK_FALSE (phazite::getAutomationTarget (context, { phazite::DeviceID::fromRaw (123456), "volume" }).has_value());

    // The master's: its volume and pan.
    const auto master = phazite::getMasterTrack (context);
    REQUIRE (master.isValid());
    const auto masterTargets = phazite::getAutomationTargets (context, master);
    REQUIRE (masterTargets.size() >= 2);
    CHECK (masterTargets[0].parameter == phazite::getMasterVolume (context).getRef());
    CHECK (masterTargets[0].name == juce::String ("Volume"));
    CHECK (phazite::showAutomationLane (context, masterTargets[0].parameter) == master);
}

TEST_CASE ("Lanes show under the track whose device they are, as view state without undo steps")
{
    Song song;
    auto& context = song.context;
    const auto a = idOf (*song.a);

    CHECK (phazite::getAutomationLanes (context, a).empty());
    CHECK (phazite::showAutomationLane (context, song.threshold()) == a);
    CHECK (phazite::showAutomationLane (context, song.volume()) == a);
    CHECK (phazite::showAutomationLane (context, song.threshold()) == a); // shown already: it stays where it is
    CHECK (lanesOf (context, a) == std::vector { song.threshold(), song.volume() });
    CHECK (phazite::getAutomationLanes (context, a)[0].height == phazite::AutomationLane::defaultHeight);
    CHECK (lanesOf (context, idOf (*song.b)).empty());
    CHECK_FALSE (song.edit->getUndoManager().canUndo());

    phazite::setAutomationLaneHeight (context, song.threshold(), 1000);
    CHECK (phazite::getAutomationLanes (context, a)[0].height == phazite::AutomationLane::maxHeight);

    // A lane that shows another parameter takes its place; the lane that showed it goes.
    phazite::setAutomationLaneParameter (context, song.threshold(), song.volume());
    CHECK (lanesOf (context, a) == std::vector { song.volume() });

    phazite::hideAutomationLane (context, song.volume());
    CHECK (lanesOf (context, a).empty());

    // A lane of a device that is gone doesn't show, and a parameter that isn't there gets none.
    const auto threshold = song.threshold();
    phazite::showAutomationLane (context, threshold);
    CHECK_FALSE (song.edit->getUndoManager().canUndo()); // none of it was an undo step
    phazite::removeDevice (context, threshold.device);
    CHECK (lanesOf (context, a).empty());
    CHECK_FALSE (phazite::showAutomationLane (context, threshold).isValid());
}

TEST_CASE ("Showing a lane opens its track, and the folders it is in")
{
    Song song;
    auto& context = song.context;
    auto* folder = phazite::groupIntoFolder ({ song.a }, nullptr);
    REQUIRE (folder != nullptr);
    phazite::setCollapsed (context, idOf (*song.a), true);
    phazite::setCollapsed (context, idOf (*folder), true);

    phazite::showAutomationLane (context, song.threshold());
    CHECK_FALSE (phazite::getTrackRow (context, idOf (*song.a))->collapsed);
    CHECK_FALSE (phazite::getTrackRow (context, idOf (*folder))->collapsed);
}

TEST_CASE ("Points are added, moved, bent, deleted and drawn in beats, each edit one named undo step")
{
    Song song;
    auto& context = song.context;
    const auto threshold = song.threshold();

    CHECK (phazite::addAutomationPoint (context, threshold, { beats (4), -30.0f }) == 0);
    CHECK (song.describeUndo() == juce::String ("Undo Add Automation Point"));
    CHECK (phazite::addAutomationPoint (context, threshold, { beats (0), -10.0f }) == 0); // before the first
    CHECK (phazite::addAutomationPoint (context, threshold, { beats (8), 20.0f }) == 2);  // its value kept in range

    auto points = phazite::getAutomationPoints (context, threshold);
    REQUIRE (points.size() == 3);
    CHECK (beatsOf (points) == std::vector { 0.0, 4.0, 8.0 });
    CHECK (points[0].value == doctest::Approx (-10.0f));
    CHECK (points[2].value == doctest::Approx (0.0f));
    CHECK (phazite::getAutomationTarget (context, threshold)->automated);

    // The values in between, as the engine plays them.
    const auto values = phazite::getAutomationValues (context, threshold, { beats (0), beats (4) }, 3);
    REQUIRE (values.size() == 3);
    CHECK (values[0] == doctest::Approx (-10.0f));
    CHECK (values[1] == doctest::Approx (-20.0f));
    CHECK (values[2] == doctest::Approx (-30.0f));

    // A point moves no further than the next one that stays, and its value stays in range.
    CHECK (phazite::moveAutomationPoints (context, threshold, { 1 }, BeatDuration::fromBeats (10.0), 100.0f).inBeats()
           == doctest::Approx (4.0));
    CHECK (song.describeUndo() == juce::String ("Undo Move Automation Points"));
    points = phazite::getAutomationPoints (context, threshold);
    CHECK (beatsOf (points) == std::vector { 0.0, 8.0, 8.0 });
    CHECK (points[1].value == doctest::Approx (0.0f));

    // Points move together, no earlier than the song's start.
    CHECK (phazite::moveAutomationPoints (context, threshold, { 0, 1 }, BeatDuration::fromBeats (-3.0), -5.0f).inBeats()
           == doctest::Approx (0.0));
    points = phazite::getAutomationPoints (context, threshold);
    CHECK (points[0].value == doctest::Approx (-15.0f));
    CHECK (points[1].value == doctest::Approx (-5.0f));

    phazite::setAutomationCurve (context, threshold, 0, 0.5f);
    CHECK (phazite::getAutomationPoints (context, threshold)[0].curve == doctest::Approx (0.5f));
    CHECK (song.describeUndo() == juce::String ("Undo Bend Automation"));

    phazite::deleteAutomationPoints (context, threshold, { 2 });
    CHECK (phazite::getAutomationPoints (context, threshold).size() == 2);
    CHECK (song.describeUndo() == juce::String ("Undo Delete Automation Points"));

    // Drawing replaces the points in a range, both its ends included.
    phazite::replaceAutomationPoints (context, threshold, { beats (0), beats (8) }, { { beats (2), -40.0f }, { beats (6), -50.0f } });
    CHECK (beatsOf (phazite::getAutomationPoints (context, threshold)) == std::vector { 2.0, 6.0 });
    CHECK (song.describeUndo() == juce::String ("Undo Draw Automation"));

    phazite::undo (*song.edit);
    CHECK (beatsOf (phazite::getAutomationPoints (context, threshold)) == std::vector { 0.0, 8.0 });
    phazite::redo (*song.edit);
    CHECK (beatsOf (phazite::getAutomationPoints (context, threshold)) == std::vector { 2.0, 6.0 });

    phazite::clearAutomation (context, threshold);
    CHECK (phazite::getAutomationPoints (context, threshold).empty());
    CHECK_FALSE (phazite::getAutomationTarget (context, threshold)->automated);
    CHECK (song.describeUndo() == juce::String ("Undo Clear Automation"));
}

TEST_CASE ("Shapes repeat a whole number of cycles across a range, starting from the first value")
{
    const phazite::BeatRange range { beats (0), beats (4) };
    const auto cycle = BeatDuration::fromBeats (1.0);

    const auto line = phazite::makeAutomationShape (AutomationShape::line, range, cycle, 0.2f, 0.8f);
    REQUIRE (line.size() == 2);
    CHECK (line[0].value == doctest::Approx (0.2f));
    CHECK (line[1].beat == beats (4));
    CHECK (line[1].value == doctest::Approx (0.8f));

    // Four cycles: up at each half beat, down at each beat, and down at the end.
    const auto triangle = phazite::makeAutomationShape (AutomationShape::triangle, range, cycle, 0.0f, 1.0f);
    REQUIRE (triangle.size() == 9);
    CHECK (triangle[1].beat.inBeats() == doctest::Approx (0.5));
    CHECK (triangle[1].value == doctest::Approx (1.0f));
    CHECK (triangle[8].beat == beats (4));
    CHECK (triangle[8].value == doctest::Approx (0.0f));

    // A square jumps: two points at each half beat.
    const auto square = phazite::makeAutomationShape (AutomationShape::square, range, cycle, 0.0f, 1.0f);
    REQUIRE (square.size() == 16);
    CHECK (square[1].beat.inBeats() == doctest::Approx (0.5));
    CHECK (square[2].beat.inBeats() == doctest::Approx (0.5));
    CHECK (square[1].value == doctest::Approx (0.0f));
    CHECK (square[2].value == doctest::Approx (1.0f));

    const auto sine = phazite::makeAutomationShape (AutomationShape::sine, range, cycle, 0.0f, 1.0f);
    REQUIRE (sine.size() == 65);
    CHECK (sine[0].value == doctest::Approx (0.0f));
    CHECK (sine[8].value == doctest::Approx (1.0f)); // half a cycle in

    // A cycle that doesn't fit is stretched to a whole number of them: 4 beats hold 3 of about 1.5.
    const auto saw = phazite::makeAutomationShape (AutomationShape::sawUp, range, BeatDuration::fromBeats (1.5), 0.0f, 1.0f);
    REQUIRE (saw.size() == 6);
    CHECK (saw[1].beat.inBeats() == doctest::Approx (4.0 / 3.0));
    CHECK (saw[1].value == doctest::Approx (1.0f));
    CHECK (saw[2].value == doctest::Approx (0.0f));
}

TEST_CASE ("Automation keeps its beats when the tempo or the time signature changes, in the same undo step")
{
    Song song;
    auto& context = song.context;
    const auto threshold = song.threshold();
    phazite::addAutomationPoint (context, threshold, { beats (4), -30.0f });
    const auto parameter = phazite::findParameter (*song.edit, threshold);
    REQUIRE (parameter != nullptr);
    CHECK (parameter->getCurve().getPointTime (0).inSeconds() == doctest::Approx (2.0)); // at 120 BPM

    phazite::setTempo (*song.edit, 60.0);
    CHECK (phazite::getAutomationPoints (context, threshold)[0].beat.inBeats() == doctest::Approx (4.0));
    CHECK (parameter->getCurve().getPointTime (0).inSeconds() == doctest::Approx (4.0));

    phazite::undo (*song.edit);
    CHECK (phazite::getTempo (*song.edit) == doctest::Approx (120.0));
    CHECK (parameter->getCurve().getPointTime (0).inSeconds() == doctest::Approx (2.0));

    phazite::setTimeSignature (*song.edit, { 3, 4 });
    CHECK (phazite::getAutomationPoints (context, threshold)[0].beat.inBeats() == doctest::Approx (4.0));
}

TEST_CASE ("Write modes are each track's, as Tracktion saves them, and changing one is no undo step")
{
    Song song;
    auto& context = song.context;
    const auto a = idOf (*song.a), b = idOf (*song.b);
    CHECK (phazite::getAutomationMode (context, a) == AutomationMode::read);

    phazite::setAutomationMode (context, a, AutomationMode::touch);
    CHECK (phazite::getAutomationMode (context, a) == AutomationMode::touch);
    CHECK (phazite::getAutomationMode (context, b) == AutomationMode::read);
    CHECK (song.a->state[te::IDs::automationMode].toString() == juce::String ("touch"));
    CHECK (phazite::isWritingAutomation (*song.edit));
    CHECK_FALSE (song.edit->getUndoManager().canUndo());

    // The commands set the selected tracks' modes.
    phazite::CommandRegistry registry;
    phazite::addAutomationCommands (registry, context);
    CHECK_FALSE (phazite::CommandRegistry::isEnabled (*registry.find ("automation.latch"))); // no track selected

    context.getSongSelection().selectOnly (a);
    context.getSongSelection().add (b);
    REQUIRE (registry.invoke ("automation.latch"));
    CHECK (phazite::getAutomationMode (context, a) == AutomationMode::latch);
    CHECK (phazite::getAutomationMode (context, b) == AutomationMode::latch);
    CHECK (registry.find ("automation.latch")->isTicked());
    CHECK_FALSE (registry.find ("automation.read")->isTicked());

    REQUIRE (registry.invoke ("automation.read"));
    CHECK_FALSE (phazite::isWritingAutomation (*song.edit));
}

TEST_CASE ("Touch writes what a control does while it is held, as one undo step")
{
    Song song;
    auto& context = song.context;
    const auto a = idOf (*song.a);
    const phazite::test::RealTimePlayback playback (phazite::test::getEngine());
    phazite::setAutomationMode (context, a, AutomationMode::touch);
    auto volume = phazite::getVolume (context, a);

    // Once the song plays (later in a slow build, such as ThreadSanitizer's).
    auto& transport = song.edit->getTransport();
    transport.setPosition (te::TimePosition());
    transport.play (false);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&transport] { return transport.getPosition() > te::TimePosition::fromSeconds (0.2); },
                                                   10000));

    volume.beginGesture();
    volume.setValue (0.3f);
    phazite::test::RealTimePlayback::wait (300);
    volume.setValue (0.6f);
    phazite::test::RealTimePlayback::wait (300);
    volume.endGesture();
    phazite::test::RealTimePlayback::wait (200);
    transport.stop (false, false);
    phazite::test::RealTimePlayback::wait (300);

    // It wrote the moves, from where the control was grabbed: the value before, then where it went.
    const auto points = phazite::getAutomationPoints (context, song.volume());
    REQUIRE (points.size() >= 2);
    CHECK (points.front().beat.inBeats() > 0.0);
    CHECK (std::any_of (points.begin(), points.end(), [] (const auto& p) { return std::abs (p.value - 0.6f) < 0.01f; }));

    // One step takes it all back.
    phazite::undo (*song.edit);
    CHECK (phazite::getAutomationPoints (context, song.volume()).empty());
}

TEST_CASE ("The parameter touched last is a control's, not automation's or a device's many at once")
{
    Song song;
    auto& context = song.context;
    const auto touched = song.threshold();
    CHECK_FALSE (phazite::getLastTouchedParameter (context).has_value());

    auto threshold = phazite::getParameter (context, touched);
    threshold.setValue (-20.0f);
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::getLastTouchedParameter (context) == touched);

    // A device added sets its parameters all at once: no one touched those.
    phazite::addEffect (context, idOf (*song.a), song.find ("phaziteEq"));
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::getLastTouchedParameter (context) == touched);

    // Automate Last Touched shows its lane, and says which it is.
    phazite::CommandRegistry registry;
    phazite::addAutomationCommands (registry, context);
    CHECK (phazite::getCurrentName (*registry.find ("automation.automateLastTouched")) == juce::String ("Automate Threshold"));
    CHECK (registry.find ("automation.automateLastTouched")->overPlugins);
    REQUIRE (registry.invoke ("automation.automateLastTouched"));
    CHECK (lanesOf (context, idOf (*song.a)) == std::vector { touched });

    // Its device gone, there is none.
    phazite::removeDevice (context, touched.device);
    CHECK_FALSE (phazite::getLastTouchedParameter (context).has_value());
    CHECK_FALSE (phazite::CommandRegistry::isEnabled (*registry.find ("automation.automateLastTouched")));
}

TEST_CASE ("Selected points go with Delete and Duplicate, and the arrow keys' commands raise and lower them")
{
    Song song;
    auto& context = song.context;
    const auto threshold = song.threshold();
    phazite::addAutomationPoint (context, threshold, { beats (0), -10.0f });
    phazite::addAutomationPoint (context, threshold, { beats (2), -20.0f });
    phazite::addAutomationPoint (context, threshold, { beats (4), -30.0f });

    auto& selection = context.getSongSelection();
    selection.selectOnly (idOf (*song.a));
    selection.selectAutomationPoints (threshold, { 2, 1 });
    CHECK (selection.getTracks().empty()); // points are selected alone
    CHECK (selection.getSelectedAutomationPoints().curve == phazite::CurveRef (threshold));
    CHECK (selection.getSelectedAutomationPoints().indices == std::vector { 1, 2 });
    CHECK (selection.isSelected (threshold, 2));
    CHECK_FALSE (selection.isSelected (threshold, 0));

    phazite::CommandRegistry registry;
    phazite::addSelectionCommands (registry, getSelection (context), getRangeSelection (context));
    phazite::addAutomationCommands (registry, context);
    CHECK (phazite::getCurrentName (*registry.find ("edit.delete")) == juce::String ("Delete 2 Automation Points"));

    // A hundredth of the range is 0.6 dB.
    REQUIRE (registry.invoke ("automation.raisePoints"));
    auto points = phazite::getAutomationPoints (context, threshold);
    CHECK (points[0].value == doctest::Approx (-10.0f));
    CHECK (points[1].value == doctest::Approx (-19.4f));
    CHECK (points[2].value == doctest::Approx (-29.4f));
    CHECK (song.describeUndo() == juce::String ("Undo Raise Automation Points"));

    // The copies go right after them, the first on the last one's beat, and are selected.
    REQUIRE (registry.invoke ("edit.duplicate"));
    points = phazite::getAutomationPoints (context, threshold);
    CHECK (beatsOf (points) == std::vector { 0.0, 2.0, 4.0, 4.0, 6.0 });
    CHECK (points[3].value == doctest::Approx (-19.4f));
    CHECK (selection.getSelectedAutomationPoints().indices == std::vector { 3, 4 });

    REQUIRE (registry.invoke ("edit.delete"));
    CHECK (beatsOf (phazite::getAutomationPoints (context, threshold)) == std::vector { 0.0, 2.0, 4.0 });
    CHECK_FALSE (selection.getSelectedAutomationPoints().curve.isValid());

    // Selecting anything else deselects them.
    selection.selectAutomationPoints (threshold, { 0 });
    selection.selectOnly (idOf (*song.b));
    CHECK_FALSE (selection.getSelectedAutomationPoints().curve.isValid());
    CHECK_FALSE (phazite::CommandRegistry::isEnabled (*registry.find ("automation.raisePoints")));
}

TEST_CASE ("Show Automation shows the selected tracks' automated lanes, or a track's volume, and hides them again")
{
    Song song;
    auto& context = song.context;
    phazite::addAutomationPoint (context, song.threshold(), { beats (0), -10.0f });

    phazite::CommandRegistry registry;
    phazite::addAutomationCommands (registry, context);
    context.getSongSelection().selectOnly (idOf (*song.a));
    context.getSongSelection().add (idOf (*song.b));

    REQUIRE (registry.invoke ("automation.showLanes"));
    CHECK (lanesOf (context, idOf (*song.a)) == std::vector { song.threshold() });
    CHECK (lanesOf (context, idOf (*song.b)) == std::vector { phazite::getVolume (context, idOf (*song.b)).getRef() });
    CHECK (registry.find ("automation.showLanes")->isTicked());

    REQUIRE (registry.invoke ("automation.showLanes"));
    CHECK (lanesOf (context, idOf (*song.a)).empty());
    CHECK (lanesOf (context, idOf (*song.b)).empty());
}

TEST_CASE ("A duplicated track's lanes show its own devices")
{
    Song song;
    auto& context = song.context;
    phazite::showAutomationLane (context, song.threshold());
    phazite::showAutomationLane (context, song.volume());

    const auto copies = phazite::duplicateTracks ({ song.a }, nullptr);
    REQUIRE (copies.size() == 1);
    const auto copy = idOf (*copies[0]);
    const auto copiedEffects = phazite::getEffects (context, copy);
    REQUIRE (copiedEffects.size() == 1);

    CHECK (lanesOf (context, copy)
           == std::vector { ParameterRef { copiedEffects[0].id, "threshold" }, phazite::getVolume (context, copy).getRef() });
    CHECK (lanesOf (context, idOf (*song.a)) == std::vector { song.threshold(), song.volume() });
}

TEST_CASE ("Lanes and the values automation plays are no unsaved changes")
{
    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (phazite::test::getEngine(), temp.folder, "Automated", error);
    REQUIRE (project != nullptr);
    phazite::test::settle();

    auto& edit = project->getEdit();
    auto* track = te::getAudioTracks (edit)[0];
    auto* volume = track->getVolumePlugin();
    REQUIRE (volume != nullptr);

    // Automation is a change; then it is saved.
    volume->volParam->getCurve().addPoint (te::TimePosition(), 0.5f, 0.0f, &edit.getUndoManager());
    CHECK (project->hasUnsavedChanges());
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (project->hasUnsavedChanges());

    // The value Tracktion writes as the automation plays isn't one, nor is a lane.
    volume->state.setProperty (te::IDs::volume, 0.25f, nullptr);
    phazite::showLane (*track, phazite::getParameterRef (*volume->volParam));
    phazite::test::settle();
    CHECK_FALSE (project->hasUnsavedChanges());

    // A parameter without automation is.
    volume->state.setProperty (te::IDs::pan, 0.5f, nullptr);
    CHECK (project->hasUnsavedChanges());
}

TEST_CASE ("A control's menu automates its parameter, and a device's menu lists the device's parameters")
{
    Song song;
    auto& context = song.context;
    const auto a = idOf (*song.a);

    // Automate shows the lane, opening the track; Clear Automation deletes the points, as one step.
    phazite::setCollapsed (context, a, true);
    auto volume = phazite::getVolume (context, a);
    CHECK_FALSE (phazite::isAutomated (volume));
    phazite::showAutomationLane (volume);
    CHECK (lanesOf (context, a) == std::vector { song.volume() });
    CHECK_FALSE (phazite::getTrackRow (context, a)->collapsed);

    phazite::addAutomationPoint (context, song.volume(), { beats (0), 0.5f });
    CHECK (phazite::isAutomated (volume));
    phazite::clearAutomation (volume);
    CHECK_FALSE (phazite::isAutomated (volume));
    CHECK (song.describeUndo() == juce::String ("Undo Clear Automation"));

    // A handle of nothing does nothing.
    phazite::showAutomationLane (phazite::ParameterHandle());
    CHECK_FALSE (phazite::isAutomated (phazite::ParameterHandle()));

    // The compressor's own parameters, by its ID.
    const auto threshold = song.threshold();
    const auto targets = phazite::getAutomationTargets (context, threshold.device);
    REQUIRE_FALSE (targets.empty());
    CHECK (std::all_of (targets.begin(), targets.end(), [&threshold] (const auto& t) { return t.parameter.device == threshold.device; }));
    CHECK (std::any_of (targets.begin(), targets.end(), [&threshold] (const auto& t) { return t.parameter == threshold; }));
    CHECK (phazite::getAutomationTargets (context, phazite::DeviceID::fromRaw (123456)).empty());
}

TEST_CASE ("The master's row is in the playlist while the master has lanes")
{
    Song song;
    auto& context = song.context;
    CHECK_FALSE (phazite::getMasterRow (context).has_value());

    phazite::showAutomationLane (context, phazite::getMasterVolume (context).getRef());
    const auto master = phazite::getMasterRow (context);
    REQUIRE (master.has_value());
    CHECK (master->id == phazite::getMasterTrack (context));
    CHECK (master->kind == phazite::TrackType::master);
    CHECK (master->name == juce::String ("Master"));

    phazite::hideAutomationLanes (context, master->id);
    CHECK_FALSE (phazite::getMasterRow (context).has_value());
}
