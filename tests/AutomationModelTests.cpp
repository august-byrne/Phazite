// SPDX-License-Identifier: AGPL-3.0-or-later
// Automation by IDs and plain values (src/model/automation/AutomationModel.h): the parameters a track offers, the lanes it
// shows as view state, points and their edits as named undo steps, shapes, automation that keeps its beats when the
// tempo changes, the write modes (what they write, on Phazite's engine: EnginePlaybackTests.cpp), the selected points'
// commands, and the parameter touched last.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/SongSelection.h"
#include "model/arrangement/internal/Selection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mirror/internal/Mirror.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>

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
        phazite::ensureTracks (*edit, 2);
        a = te::getAudioTracks (*edit)[0];
        b = te::getAudioTracks (*edit)[1];
        phazite::setTrackName (*a, "A");
        phazite::setTrackName (*b, "B");
        setEdit (context, edit.get());
        phazite::chooseInstrument (context, idOf (*a), find ("phaziteSynth"));
        phazite::addEffect (context, idOf (*a), find ("phaziteCompressor"));
        phazite::getSong (*edit).getUndoManager().clearUndoHistory();
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

    // A's compressor's makeup gain, from -12 to 24 dB, and A's volume.
    ParameterRef makeup() const
    {
        const auto effects = phazite::getEffects (context, idOf (*a));
        REQUIRE (effects.size() == 1);
        return { effects[0].id, "makeup" };
    }

    ParameterRef volume() { return phazite::getVolume (context, idOf (*a)).getRef(); }

    juce::String describeUndo() const { return phazite::describeUndo (edit.get()); }

    std::unique_ptr<te::Edit> edit = phazite::createSongEdit (phazite::test::getEngine());
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

    // The compressor's makeup gain among its devices', none of them a device's dry or wet level (its mix is one knob).
    const auto makeup = std::find_if (targets.begin(), targets.end(), [&song] (const auto& t) { return t.parameter == song.makeup(); });
    REQUIRE (makeup != targets.end());
    CHECK (makeup->name == juce::String ("Makeup"));
    CHECK (makeup->deviceName == juce::String ("Compressor"));
    CHECK (makeup->track == idOf (*song.a));
    CHECK (makeup->kind == ParameterKind::other);
    CHECK_FALSE (makeup->automated);
    CHECK (std::none_of (targets.begin(), targets.end(),
                         [] (const auto& t) { return t.name.containsIgnoreCase ("dry") || t.name.containsIgnoreCase ("wet"); }));

    // Each by its ref, and as a handle to change it.
    CHECK (phazite::getAutomationTarget (context, song.makeup())->name == juce::String ("Makeup"));
    CHECK (phazite::getParameter (context, song.makeup()).getName() == juce::String ("Makeup"));
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
    CHECK (phazite::showAutomationLane (context, song.makeup()) == a);
    CHECK (phazite::showAutomationLane (context, song.volume()) == a);
    CHECK (phazite::showAutomationLane (context, song.makeup()) == a); // shown already: it stays where it is
    CHECK (lanesOf (context, a) == std::vector { song.makeup(), song.volume() });
    CHECK (phazite::getAutomationLanes (context, a)[0].height == phazite::AutomationLane::defaultHeight);
    CHECK (lanesOf (context, idOf (*song.b)).empty());
    CHECK_FALSE (phazite::getSong (*song.edit).getUndoManager().canUndo());

    phazite::setAutomationLaneHeight (context, song.makeup(), 1000);
    CHECK (phazite::getAutomationLanes (context, a)[0].height == phazite::AutomationLane::maxHeight);

    // A lane that shows another parameter takes its place; the lane that showed it goes.
    phazite::setAutomationLaneParameter (context, song.makeup(), song.volume());
    CHECK (lanesOf (context, a) == std::vector { song.volume() });

    // A lane without automation goes at once.
    phazite::removeAutomationLane (context, song.volume());
    CHECK (lanesOf (context, a).empty());

    // A lane of a device that is gone doesn't show, and a parameter that isn't there gets none.
    const auto makeup = song.makeup();
    phazite::showAutomationLane (context, makeup);
    CHECK_FALSE (phazite::getSong (*song.edit).getUndoManager().canUndo()); // none of it was an undo step
    phazite::removeDevice (context, makeup.device);
    CHECK (lanesOf (context, a).empty());
    CHECK_FALSE (phazite::showAutomationLane (context, makeup).isValid());
}

TEST_CASE ("Showing a lane opens its track, and the folders it is in")
{
    Song song;
    auto& context = song.context;
    auto* folder = phazite::groupIntoFolder ({ song.a }, nullptr);
    REQUIRE (folder != nullptr);
    phazite::setCollapsed (context, idOf (*song.a), true);
    phazite::setCollapsed (context, idOf (*folder), true);

    phazite::showAutomationLane (context, song.makeup());
    CHECK_FALSE (phazite::getTrackRow (context, idOf (*song.a))->collapsed);
    CHECK_FALSE (phazite::getTrackRow (context, idOf (*folder))->collapsed);
}

TEST_CASE ("Automation never hides: every automated parameter has a lane, which collapses to a thin row")
{
    Song song;
    auto& context = song.context;
    const auto a = idOf (*song.a);
    const auto makeup = song.makeup(), volume = song.volume();

    // Automation written where no lane showed (as recording, or an older project, gives it) shows one after the others.
    phazite::showAutomationLane (context, volume);
    phazite::addAutomationPoint (context, makeup, { beats (4), -9.0f });
    CHECK (lanesOf (context, a) == std::vector { volume, makeup });
    phazite::getSong (*song.edit).getUndoManager().clearUndoHistory();

    // A lane collapses and expands where it is; showing it expands it too. None of it is an undo step.
    phazite::setAutomationLaneCollapsed (context, makeup, true);
    auto lanes = phazite::getAutomationLanes (context, a);
    REQUIRE (lanes.size() == 2);
    CHECK (lanes[1].parameter == makeup);
    CHECK (lanes[1].collapsed);
    CHECK_FALSE (lanes[0].collapsed);
    phazite::setAutomationLaneCollapsed (context, volume, true);
    CHECK (lanesOf (context, a) == std::vector { volume, makeup });
    phazite::showAutomationLane (context, makeup);
    CHECK_FALSE (phazite::getAutomationLanes (context, a)[1].collapsed);
    phazite::setAutomationLanesCollapsed (context, a, false);
    CHECK_FALSE (phazite::getAutomationLanes (context, a)[0].collapsed);
    CHECK_FALSE (phazite::getSong (*song.edit).getUndoManager().canUndo());

    // Removing a lane with automation deletes the automation, as one step that brings both back.
    phazite::removeAutomationLane (context, makeup);
    CHECK (lanesOf (context, a) == std::vector { volume });
    CHECK (phazite::getAutomationPoints (context, makeup).empty());
    CHECK (song.describeUndo() == juce::String ("Undo Delete Automation"));
    phazite::undo (*song.edit);
    CHECK (lanesOf (context, a) == std::vector { volume, makeup });
    CHECK (phazite::getAutomationPoints (context, makeup).size() == 1);
}

TEST_CASE ("Show Automation expands the selected tracks' lanes, or collapses them all while one shows expanded")
{
    Song song;
    auto& context = song.context;
    const auto a = idOf (*song.a);
    phazite::CommandRegistry registry;
    phazite::addAutomationCommands (registry, context);
    context.getSongSelection().selectOnly (a);

    // A track without lanes gets one for its volume, expanded; again, its lanes collapse.
    REQUIRE (registry.invoke ("automation.showLanes"));
    CHECK (lanesOf (context, a) == std::vector { song.volume() });
    CHECK (registry.find ("automation.showLanes")->isTicked());
    REQUIRE (registry.invoke ("automation.showLanes"));
    CHECK (phazite::getAutomationLanes (context, a)[0].collapsed);
    CHECK_FALSE (registry.find ("automation.showLanes")->isTicked());

    // Expanding them opens their collapsed track.
    phazite::setCollapsed (context, a, true);
    REQUIRE (registry.invoke ("automation.showLanes"));
    CHECK_FALSE (phazite::getAutomationLanes (context, a)[0].collapsed);
    CHECK_FALSE (phazite::getTrackRow (context, a)->collapsed);
}

TEST_CASE ("Points are added, moved, bent, deleted and drawn in beats, each edit one named undo step")
{
    Song song;
    auto& context = song.context;
    const auto makeup = song.makeup();

    CHECK (phazite::addAutomationPoint (context, makeup, { beats (4), -10.0f }) == 0);
    CHECK (song.describeUndo() == juce::String ("Undo Add Automation Point"));
    CHECK (phazite::addAutomationPoint (context, makeup, { beats (0), 10.0f }) == 0); // before the first
    CHECK (phazite::addAutomationPoint (context, makeup, { beats (8), 40.0f }) == 2); // its value kept in range

    auto points = phazite::getAutomationPoints (context, makeup);
    REQUIRE (points.size() == 3);
    CHECK (beatsOf (points) == std::vector { 0.0, 4.0, 8.0 });
    CHECK (points[0].value == doctest::Approx (10.0f));
    CHECK (points[2].value == doctest::Approx (24.0f));
    CHECK (phazite::getAutomationTarget (context, makeup)->automated);

    // The values in between, as the engine plays them.
    const auto values = phazite::getAutomationValues (context, makeup, { beats (0), beats (4) }, 3);
    REQUIRE (values.size() == 3);
    CHECK (values[0] == doctest::Approx (10.0f));
    CHECK (values[1] == doctest::Approx (0.0f));
    CHECK (values[2] == doctest::Approx (-10.0f));

    // A point moves no further than the next one that stays, and its value stays in range.
    CHECK (phazite::moveAutomationPoints (context, makeup, { 1 }, BeatDuration::fromBeats (10.0), 100.0f).inBeats()
           == doctest::Approx (4.0));
    CHECK (song.describeUndo() == juce::String ("Undo Move Automation Points"));
    points = phazite::getAutomationPoints (context, makeup);
    CHECK (beatsOf (points) == std::vector { 0.0, 8.0, 8.0 });
    CHECK (points[1].value == doctest::Approx (24.0f));

    // Points move together, no earlier than the song's start.
    CHECK (phazite::moveAutomationPoints (context, makeup, { 0, 1 }, BeatDuration::fromBeats (-3.0), -5.0f).inBeats()
           == doctest::Approx (0.0));
    points = phazite::getAutomationPoints (context, makeup);
    CHECK (points[0].value == doctest::Approx (5.0f));
    CHECK (points[1].value == doctest::Approx (19.0f));

    phazite::setAutomationCurve (context, makeup, 0, 0.5f);
    CHECK (phazite::getAutomationPoints (context, makeup)[0].curve == doctest::Approx (0.5f));
    CHECK (song.describeUndo() == juce::String ("Undo Bend Automation"));

    phazite::deleteAutomationPoints (context, makeup, { 2 });
    CHECK (phazite::getAutomationPoints (context, makeup).size() == 2);
    CHECK (song.describeUndo() == juce::String ("Undo Delete Automation Points"));

    // Drawing replaces the points in a range, both its ends included.
    phazite::replaceAutomationPoints (context, makeup, { beats (0), beats (8) }, { { beats (2), -6.0f }, { beats (6), -12.0f } });
    CHECK (beatsOf (phazite::getAutomationPoints (context, makeup)) == std::vector { 2.0, 6.0 });
    CHECK (song.describeUndo() == juce::String ("Undo Draw Automation"));

    phazite::undo (*song.edit);
    CHECK (beatsOf (phazite::getAutomationPoints (context, makeup)) == std::vector { 0.0, 8.0 });
    phazite::redo (*song.edit);
    CHECK (beatsOf (phazite::getAutomationPoints (context, makeup)) == std::vector { 2.0, 6.0 });

    phazite::removeAutomationLane (context, makeup);
    CHECK (phazite::getAutomationPoints (context, makeup).empty());
    CHECK_FALSE (phazite::getAutomationTarget (context, makeup)->automated);
    CHECK (song.describeUndo() == juce::String ("Undo Delete Automation"));
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
    const auto makeup = song.makeup();
    phazite::addAutomationPoint (context, makeup, { beats (4), -9.0f });
    const auto parameter = phazite::findParameter (*song.edit, makeup);
    REQUIRE (parameter != nullptr);
    CHECK (parameter->getCurve().getPointTime (0).inSeconds() == doctest::Approx (2.0)); // at 120 BPM

    phazite::setTempo (*song.edit, 60.0);
    CHECK (phazite::getAutomationPoints (context, makeup)[0].beat.inBeats() == doctest::Approx (4.0));
    CHECK (parameter->getCurve().getPointTime (0).inSeconds() == doctest::Approx (4.0));

    phazite::undo (*song.edit);
    CHECK (phazite::getTempo (phazite::getSong (*song.edit)) == doctest::Approx (120.0));
    CHECK (parameter->getCurve().getPointTime (0).inSeconds() == doctest::Approx (2.0));

    phazite::setTimeSignature (phazite::getSong (*song.edit), { 3, 4 });
    CHECK (phazite::getAutomationPoints (context, makeup)[0].beat.inBeats() == doctest::Approx (4.0));
}

TEST_CASE ("Write modes are each track's, the song's, which Tracktion's tracks mirror, and changing one is no undo step")
{
    Song song;
    auto& context = song.context;
    const auto a = idOf (*song.a), b = idOf (*song.b);
    CHECK (phazite::getAutomationMode (context, a) == AutomationMode::read);

    phazite::setAutomationMode (context, a, AutomationMode::touch);
    CHECK (phazite::getAutomationMode (context, a) == AutomationMode::touch);
    CHECK (phazite::getAutomationMode (context, b) == AutomationMode::read);
    CHECK (phazite::findTrackState (*song.a)[phazite::schema::trackAutomationMode].toString() == juce::String ("touch"));
    CHECK ((song.a->automationMode.get() == te::AutomationMode::touch)); // Tracktion's toString() for it isn't doctest's
    CHECK (phazite::isWritingAutomation (*song.edit));
    CHECK_FALSE (phazite::getSong (*song.edit).getUndoManager().canUndo());

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

TEST_CASE ("The parameter touched last is a control's, not automation's or a device's many at once")
{
    Song song;
    auto& context = song.context;
    const auto touched = song.makeup();
    CHECK_FALSE (phazite::getLastTouchedParameter (context).has_value());

    auto makeup = phazite::getParameter (context, touched);
    makeup.setValue (-20.0f);
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::getLastTouchedParameter (context) == touched);

    // A device added sets its parameters all at once: no one touched those.
    phazite::addEffect (context, idOf (*song.a), song.find ("phaziteEq"));
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::getLastTouchedParameter (context) == touched);

    // Automate Last Touched shows its lane, and says which it is.
    phazite::CommandRegistry registry;
    phazite::addAutomationCommands (registry, context);
    CHECK (phazite::getCurrentName (*registry.find ("automation.automateLastTouched")) == juce::String ("Automate Makeup"));
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
    const auto makeup = song.makeup();
    phazite::addAutomationPoint (context, makeup, { beats (0), -3.0f });
    phazite::addAutomationPoint (context, makeup, { beats (2), -6.0f });
    phazite::addAutomationPoint (context, makeup, { beats (4), -9.0f });

    auto& selection = context.getSongSelection();
    selection.selectOnly (idOf (*song.a));
    selection.selectAutomationPoints (makeup, { 2, 1 });
    CHECK (selection.getTracks().empty()); // points are selected alone
    CHECK (selection.getSelectedAutomationPoints().parameter == makeup);
    CHECK (selection.getSelectedAutomationPoints().indices == std::vector { 1, 2 });
    CHECK (selection.isSelected (makeup, 2));
    CHECK_FALSE (selection.isSelected (makeup, 0));

    phazite::CommandRegistry registry;
    phazite::addSelectionCommands (registry, getSelection (context), getRangeSelection (context));
    phazite::addAutomationCommands (registry, context);
    CHECK (phazite::getCurrentName (*registry.find ("edit.delete")) == juce::String ("Delete 2 Automation Points"));

    // A hundredth of the range is 0.36 dB.
    REQUIRE (registry.invoke ("automation.raisePoints"));
    auto points = phazite::getAutomationPoints (context, makeup);
    CHECK (points[0].value == doctest::Approx (-3.0f));
    CHECK (points[1].value == doctest::Approx (-5.64f));
    CHECK (points[2].value == doctest::Approx (-8.64f));
    CHECK (song.describeUndo() == juce::String ("Undo Raise Automation Points"));

    // The copies go right after them, the first on the last one's beat, and are selected.
    REQUIRE (registry.invoke ("edit.duplicate"));
    points = phazite::getAutomationPoints (context, makeup);
    CHECK (beatsOf (points) == std::vector { 0.0, 2.0, 4.0, 4.0, 6.0 });
    CHECK (points[3].value == doctest::Approx (-5.64f));
    CHECK (selection.getSelectedAutomationPoints().indices == std::vector { 3, 4 });

    REQUIRE (registry.invoke ("edit.delete"));
    CHECK (beatsOf (phazite::getAutomationPoints (context, makeup)) == std::vector { 0.0, 2.0, 4.0 });
    CHECK_FALSE (selection.getSelectedAutomationPoints().parameter.isValid());

    // Selecting anything else deselects them.
    selection.selectAutomationPoints (makeup, { 0 });
    selection.selectOnly (idOf (*song.b));
    CHECK_FALSE (selection.getSelectedAutomationPoints().parameter.isValid());
    CHECK_FALSE (phazite::CommandRegistry::isEnabled (*registry.find ("automation.raisePoints")));
}

TEST_CASE ("Show Automation collapses the selected tracks' lanes while one shows expanded, and expands them again")
{
    Song song;
    auto& context = song.context;
    const auto a = idOf (*song.a), b = idOf (*song.b);
    phazite::addAutomationPoint (context, song.makeup(), { beats (0), -3.0f });

    phazite::CommandRegistry registry;
    phazite::addAutomationCommands (registry, context);
    context.getSongSelection().selectOnly (a);
    context.getSongSelection().add (b);

    // A's automation shows a lane, expanded: the selected tracks' lanes collapse.
    CHECK (lanesOf (context, a) == std::vector { song.makeup() });
    CHECK (registry.find ("automation.showLanes")->isTicked());
    REQUIRE (registry.invoke ("automation.showLanes"));
    CHECK (phazite::getAutomationLanes (context, a)[0].collapsed);
    CHECK (lanesOf (context, b).empty());
    CHECK_FALSE (registry.find ("automation.showLanes")->isTicked());

    // Again: they expand, and B, without lanes, gets one for its volume.
    REQUIRE (registry.invoke ("automation.showLanes"));
    CHECK_FALSE (phazite::getAutomationLanes (context, a)[0].collapsed);
    CHECK (lanesOf (context, b) == std::vector { phazite::getVolume (context, b).getRef() });
    CHECK (registry.find ("automation.showLanes")->isTicked());
}

TEST_CASE ("Automation is the song's own, which Tracktion's curves mirror one way, undo and redo too")
{
    Song song;
    auto& edit = *song.edit;
    const auto makeup = song.makeup();
    const auto parameter = phazite::findParameter (edit, makeup);
    REQUIRE (parameter != nullptr);
    const auto& curve = parameter->getCurve();

    // A point goes into the song's curve for the parameter, in beats, and Tracktion's follows, in seconds at 120 BPM.
    phazite::addAutomationPoint (song.context, makeup, { beats (4), -9.0f });
    const auto state = phazite::findCurveState (edit, makeup);
    REQUIRE (state.isValid());
    CHECK (state.getParent().hasType (phazite::schema::AUTOMATION));
    CHECK (phazite::getCurveParameter (state) == makeup);
    REQUIRE (state.getNumChildren() == 1);
    CHECK ((double) state.getChild (0)[phazite::schema::pointBeat] == doctest::Approx (4.0));
    CHECK ((float) state.getChild (0)[phazite::schema::pointValue] == doctest::Approx (-9.0f));
    REQUIRE (curve.getNumPoints() == 1);
    CHECK (curve.getPointTime (0).inSeconds() == doctest::Approx (2.0));
    CHECK (curve.getPointValue (0) == doctest::Approx (-9.0f));

    // An undo takes both away, the song's curve with its last point, and a redo puts both back.
    phazite::undo (edit);
    CHECK_FALSE (phazite::findCurveState (edit, makeup).isValid());
    CHECK (curve.getNumPoints() == 0);
    phazite::redo (edit);
    CHECK (phazite::getAutomationPoints (song.context, makeup).size() == 1);
    CHECK (curve.getNumPoints() == 1);

    // One way: a point in Tracktion's curve is none of the song's, and the mirror takes it out, as it empties a curve of
    // Tracktion's that the song has none for. Once they agree, it changes nothing.
    parameter->getCurve().addPoint (te::TimePosition::fromSeconds (3.0), -10.0f, 0.0f, nullptr);
    const auto volume = phazite::findParameter (edit, song.volume());
    REQUIRE (volume != nullptr);
    volume->getCurve().addPoint (te::TimePosition(), 0.5f, 0.0f, nullptr);
    CHECK (phazite::getAutomationPoints (song.context, makeup).size() == 1);
    CHECK (phazite::getAutomationPoints (song.context, song.volume()).empty());

    phazite::mirror::reconcileAutomation (edit);
    CHECK (curve.getNumPoints() == 1);
    CHECK (volume->getCurve().getNumPoints() == 0);

    phazite::getSong (edit).getUndoManager().clearUndoHistory();
    phazite::mirror::reconcileAutomation (edit);
    CHECK_FALSE (phazite::getSong (edit).getUndoManager().canUndo());
}

TEST_CASE ("A control turned moves its curve's one point, the song's too, in the control's undo step")
{
    // As Tracktion moves it, with the song playing or not (it has a playback context then).
    Song song;
    auto& context = song.context;
    song.edit->getTransport().ensureContextAllocated();
    phazite::addAutomationPoint (context, song.volume(), { beats (2), 0.5f });
    phazite::getSong (*song.edit).getUndoManager().clearUndoHistory();

    auto volume = phazite::getVolume (context, idOf (*song.a));
    volume.beginGesture();
    volume.setValue (0.7f);
    volume.endGesture();

    auto points = phazite::getAutomationPoints (context, song.volume());
    REQUIRE (points.size() == 1);
    CHECK (points[0].beat.inBeats() == doctest::Approx (2.0));
    CHECK (points[0].value == doctest::Approx (0.7f));

    phazite::undo (*song.edit);
    points = phazite::getAutomationPoints (context, song.volume());
    REQUIRE (points.size() == 1);
    CHECK (points[0].value == doctest::Approx (0.5f));
    CHECK (phazite::findParameter (*song.edit, song.volume())->getCurve().getPointValue (0) == doctest::Approx (0.5f));
}

TEST_CASE ("A duplicated track's automation is its own devices', and the original's stays")
{
    Song song;
    auto& context = song.context;
    phazite::addAutomationPoint (context, song.makeup(), { beats (4), -9.0f });
    phazite::addAutomationPoint (context, song.volume(), { beats (2), 0.25f });

    juce::Array<te::Track*> copies;

    {
        const phazite::UndoStep step (*song.edit, "Duplicate Track");
        copies = phazite::duplicateTracks ({ song.a }, nullptr);
    }

    REQUIRE (copies.size() == 1);
    const auto copy = idOf (*copies[0]);
    const auto copiedEffects = phazite::getEffects (context, copy);
    REQUIRE (copiedEffects.size() == 1);
    const ParameterRef copiedMakeup { copiedEffects[0].id, "makeup" };
    const auto copiedVolume = phazite::getVolume (context, copy).getRef();

    for (const auto& [original, copied] : { std::pair (song.makeup(), copiedMakeup), std::pair (song.volume(), copiedVolume) })
    {
        const auto points = phazite::getAutomationPoints (context, original);
        REQUIRE (points.size() == 1);
        const auto copiedPoints = phazite::getAutomationPoints (context, copied);
        REQUIRE (copiedPoints.size() == 1);
        CHECK (copiedPoints[0].beat.inBeats() == doctest::Approx (points[0].beat.inBeats()));
        CHECK (copiedPoints[0].value == doctest::Approx (points[0].value));
        CHECK (phazite::findParameter (*song.edit, copied)->getCurve().getNumPoints() == 1);
    }

    // The copy's curves go with it in one undo.
    phazite::undo (*song.edit);
    CHECK_FALSE (phazite::findCurveState (*song.edit, copiedMakeup).isValid());
    CHECK (phazite::getAutomationPoints (context, song.makeup()).size() == 1);
}

TEST_CASE ("A duplicated track's lanes show its own devices")
{
    Song song;
    auto& context = song.context;
    phazite::showAutomationLane (context, song.makeup());
    phazite::showAutomationLane (context, song.volume());

    const auto copies = phazite::duplicateTracks ({ song.a }, nullptr);
    REQUIRE (copies.size() == 1);
    const auto copy = idOf (*copies[0]);
    const auto copiedEffects = phazite::getEffects (context, copy);
    REQUIRE (copiedEffects.size() == 1);

    CHECK (lanesOf (context, copy)
           == std::vector { ParameterRef { copiedEffects[0].id, "makeup" }, phazite::getVolume (context, copy).getRef() });
    CHECK (lanesOf (context, idOf (*song.a)) == std::vector { song.makeup(), song.volume() });
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
    phazite::addPoint (*volume->volParam, { phazite::BeatPosition(), 0.5f, 0.0f });
    CHECK (project->hasUnsavedChanges());
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (project->hasUnsavedChanges());

    // The value Tracktion writes as the automation plays isn't one, nor is a lane.
    volume->state.setProperty (te::IDs::volume, 0.25f, nullptr);
    phazite::showLane (*track, phazite::getParameterRef (*volume->volParam));
    phazite::test::settle();
    CHECK_FALSE (project->hasUnsavedChanges());

    // A value set by hand is: the song's (Schema.h: channelPan), not what Tracktion's fader keeps of it.
    phazite::setChannelPan (*track, 0.5f);
    CHECK (project->hasUnsavedChanges());
}

TEST_CASE ("A control's menu automates its parameter, and a device's menu lists the device's parameters")
{
    Song song;
    auto& context = song.context;
    const auto a = idOf (*song.a);

    // Automate shows the lane, opening the track; Delete Automation deletes the points as one step, and the lane.
    phazite::setCollapsed (context, a, true);
    auto volume = phazite::getVolume (context, a);
    CHECK_FALSE (phazite::isAutomated (volume));
    phazite::showAutomationLane (volume);
    CHECK (lanesOf (context, a) == std::vector { song.volume() });
    CHECK_FALSE (phazite::getTrackRow (context, a)->collapsed);

    phazite::addAutomationPoint (context, song.volume(), { beats (0), 0.5f });
    CHECK (phazite::isAutomated (volume));
    phazite::deleteAutomation (volume);
    CHECK_FALSE (phazite::isAutomated (volume));
    CHECK (lanesOf (context, a).empty());
    CHECK (song.describeUndo() == juce::String ("Undo Delete Automation"));

    // A handle of nothing does nothing.
    phazite::showAutomationLane (phazite::ParameterHandle());
    CHECK_FALSE (phazite::isAutomated (phazite::ParameterHandle()));

    // The compressor's own parameters, by its ID.
    const auto makeup = song.makeup();
    const auto targets = phazite::getAutomationTargets (context, makeup.device);
    REQUIRE_FALSE (targets.empty());
    CHECK (std::all_of (targets.begin(), targets.end(), [&makeup] (const auto& t) { return t.parameter.device == makeup.device; }));
    CHECK (std::any_of (targets.begin(), targets.end(), [&makeup] (const auto& t) { return t.parameter == makeup; }));
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

    phazite::removeAutomationLane (context, phazite::getMasterVolume (context).getRef());
    CHECK_FALSE (phazite::getMasterRow (context).has_value());
}

TEST_CASE ("A return's row is in the playlist while the return has lanes, after the tracks, as the master's is")
{
    Song song;
    auto& context = song.context;
    auto* returnTrack = phazite::addReturnTrack (*song.edit, "Reverb", nullptr);
    REQUIRE (returnTrack != nullptr);
    const auto id = idOf (*returnTrack);

    // It holds no clips, and has no lanes yet: no row (the mixer shows it).
    const auto hasRow = [&context, id]
    {
        const auto rows = phazite::getPlaylistRows (context);
        return std::any_of (rows.begin(), rows.end(), [id] (const phazite::TrackRow& row) { return row.id == id; });
    };

    CHECK_FALSE (hasRow());
    CHECK (phazite::getTrackRow (context, id).has_value()); // still the playlist's, as a track in a collapsed folder is

    // With a lane, it shows after the tracks, wherever it is in the song's list of tracks.
    phazite::moveTracksTo (context, { id }, phazite::getAllPlaylistTracks (context).front(), phazite::TrackDrop::before);
    REQUIRE (phazite::getAllPlaylistTracks (context).front() == id);
    phazite::showAutomationLane (context, phazite::getVolume (context, id).getRef());
    const auto rows = phazite::getPlaylistRows (context);
    REQUIRE (rows.size() == 3);
    CHECK (rows.back().id == id);
    CHECK (rows.back().kind == phazite::TrackType::returnTrack);

    phazite::removeAutomationLane (context, phazite::getVolume (context, id).getRef());
    CHECK_FALSE (hasRow());
}
