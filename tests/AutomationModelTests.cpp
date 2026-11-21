// SPDX-License-Identifier: AGPL-3.0-or-later
// Automation by IDs and plain values (src/model/automation/AutomationModel.h): the parameters a track offers, the lanes it
// shows as view state, points and their edits as named undo steps, shapes, automation that keeps its beats when the
// tempo changes, the write modes (what they write, on Phazite's engine: EnginePlaybackTests.cpp), the selected points'
// commands, and the parameter touched last.
#include "EnginePlayback.h"
#include "GoldenFiles.h"
#include "SongCheck.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/SongSelection.h"
#include "model/arrangement/internal/Selection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/playback/internal/SongPlayer.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <vector>

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

// An open project with two tracks, A (with a synth, then a compressor) and B, as the app has it, and no undo history.
struct Song
{
    Song()
    {
        const auto tracks = phazite::ensureTracks (*song, 2);
        a = tracks[0];
        b = tracks[1];
        phazite::setTrackName (*song, a, "A");
        phazite::setTrackName (*song, b, "B");
        phazite::setSong (context, song.get());
        phazite::chooseInstrument (context, a, find ("phaziteSynth"));
        phazite::addEffect (context, a, find ("phaziteCompressor"));
        song->getUndoManager().clearUndoHistory();
    }

    ~Song() { phazite::setSong (context, nullptr); }

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
        const auto effects = phazite::getEffects (context, a);
        REQUIRE (effects.size() == 1);
        return { effects[0].id, "makeup" };
    }

    ParameterRef volume() { return phazite::getVolume (context, a).getRef(); }

    juce::String describeUndo() const { return phazite::describeUndo (song.get()); }

    phazite::Song::Ptr song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::TrackID a, b;
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
    const auto targets = phazite::getAutomationTargets (context, song.a);
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
    CHECK (makeup->track == song.a);
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
    const auto a = song.a;

    CHECK (phazite::getAutomationLanes (context, a).empty());
    CHECK (phazite::showAutomationLane (context, song.makeup()) == a);
    CHECK (phazite::showAutomationLane (context, song.volume()) == a);
    CHECK (phazite::showAutomationLane (context, song.makeup()) == a); // shown already: it stays where it is
    CHECK (lanesOf (context, a) == std::vector { song.makeup(), song.volume() });
    CHECK (phazite::getAutomationLanes (context, a)[0].height == phazite::AutomationLane::defaultHeight);
    CHECK (lanesOf (context, song.b).empty());
    CHECK_FALSE (song.song->getUndoManager().canUndo());

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
    CHECK_FALSE (song.song->getUndoManager().canUndo()); // none of it was an undo step
    phazite::removeDevice (context, makeup.device);
    CHECK (lanesOf (context, a).empty());
    CHECK_FALSE (phazite::showAutomationLane (context, makeup).isValid());
}

TEST_CASE ("Showing a lane opens its track, and the folders it is in")
{
    Song song;
    auto& context = song.context;
    const auto folder = phazite::groupIntoFolder (*song.song, { song.a }, nullptr);
    REQUIRE (folder.isValid());
    phazite::setCollapsed (context, song.a, true);
    phazite::setCollapsed (context, folder, true);

    phazite::showAutomationLane (context, song.makeup());
    CHECK_FALSE (phazite::getTrackRow (context, song.a)->collapsed);
    CHECK_FALSE (phazite::getTrackRow (context, folder)->collapsed);
}

TEST_CASE ("Automation never hides: every automated parameter has a lane, which collapses to a thin row")
{
    Song song;
    auto& context = song.context;
    const auto a = song.a;
    const auto makeup = song.makeup(), volume = song.volume();

    // Automation written where no lane showed (as recording, or an older project, gives it) shows one after the others.
    phazite::showAutomationLane (context, volume);
    phazite::addAutomationPoint (context, makeup, { beats (4), 0.25f });
    CHECK (lanesOf (context, a) == std::vector { volume, makeup });
    song.song->getUndoManager().clearUndoHistory();

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
    CHECK_FALSE (song.song->getUndoManager().canUndo());

    // Removing a lane with automation deletes the automation, as one step that brings both back.
    phazite::removeAutomationLane (context, makeup);
    CHECK (lanesOf (context, a) == std::vector { volume });
    CHECK (phazite::getAutomationPoints (context, makeup).empty());
    CHECK (song.describeUndo() == juce::String ("Undo Delete Automation"));
    phazite::undo (*song.song);
    CHECK (lanesOf (context, a) == std::vector { volume, makeup });
    CHECK (phazite::getAutomationPoints (context, makeup).size() == 1);
}

TEST_CASE ("A lane shown or removed leaves the undo history's clips and sends where it finds them")
{
    // Lanes are view state, changed outside the undo history, which finds a track's clips and sends by their places among
    // the track's elements: a lane removed from among them made Undo take the wrong clip, or none.
    Song song;
    auto& context = song.context;
    const auto a = song.a;
    const auto volume = song.volume();
    const auto clipsOf = [&context, a]
    {
        std::vector<phazite::ClipID> ids;

        for (const auto& clip : phazite::getClips (context, a))
            ids.push_back (clip.id);

        return ids;
    };

    // A lane, then two clips and a send, each a step; then the lane goes (Delete Automation, with no points to delete).
    phazite::showAutomationLane (context, volume);
    const auto first = phazite::addMidiClipAt (context, a, beats (0));
    const auto second = phazite::addMidiClipAt (context, a, beats (8));
    REQUIRE (first.isValid());
    REQUIRE (second.isValid());
    phazite::addSendToNewReturn (context, a);
    REQUIRE (phazite::getSends (context, a).size() == 1);

    phazite::removeAutomationLane (context, volume);
    CHECK (lanesOf (context, a).empty());

    // Each undo takes back its own step, and each redo brings it back.
    phazite::undo (*song.song);
    CHECK (phazite::getSends (context, a).empty());
    CHECK (clipsOf() == std::vector { first, second });
    phazite::undo (*song.song);
    CHECK (clipsOf() == std::vector { first });
    phazite::undo (*song.song);
    CHECK (clipsOf().empty());
    CHECK_FALSE (song.song->getUndoManager().canUndo());

    // A lane shown while the steps are undone doesn't move them either.
    phazite::showAutomationLane (context, song.makeup());
    phazite::redo (*song.song);
    CHECK (clipsOf() == std::vector { first });
    phazite::redo (*song.song);
    CHECK (clipsOf() == std::vector { first, second });
    phazite::removeAutomationLane (context, song.makeup());
    phazite::undo (*song.song);
    CHECK (clipsOf() == std::vector { first });
}

TEST_CASE ("Show Automation expands the selected tracks' lanes, or collapses them all while one shows expanded")
{
    Song song;
    auto& context = song.context;
    const auto a = song.a;
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

    CHECK (phazite::addAutomationPoint (context, makeup, { beats (4), 0.1f }) == 0);
    CHECK (song.describeUndo() == juce::String ("Undo Add Automation Point"));
    CHECK (phazite::addAutomationPoint (context, makeup, { beats (0), 0.7f }) == 0); // before the first
    CHECK (phazite::addAutomationPoint (context, makeup, { beats (8), 1.5f }) == 2); // kept on the control's travel

    auto points = phazite::getAutomationPoints (context, makeup);
    REQUIRE (points.size() == 3);
    CHECK (beatsOf (points) == std::vector { 0.0, 4.0, 8.0 });
    CHECK (points[0].position == doctest::Approx (0.7f));
    CHECK (points[2].position == doctest::Approx (1.0f));
    CHECK (phazite::getAutomationTarget (context, makeup)->automated);

    // The positions in between, as the engine plays them.
    const auto positions = phazite::getAutomationPositions (context, makeup, { beats (0), beats (4) }, 3);
    REQUIRE (positions.size() == 3);
    CHECK (positions[0] == doctest::Approx (0.7f));
    CHECK (positions[1] == doctest::Approx (0.4f));
    CHECK (positions[2] == doctest::Approx (0.1f));

    // A point moves no further than the next one that stays, and stays on the control's travel.
    CHECK (phazite::moveAutomationPoints (context, makeup, { 1 }, BeatDuration::fromBeats (10.0), 2.0f).inBeats() == doctest::Approx (4.0));
    CHECK (song.describeUndo() == juce::String ("Undo Move Automation Points"));
    points = phazite::getAutomationPoints (context, makeup);
    CHECK (beatsOf (points) == std::vector { 0.0, 8.0, 8.0 });
    CHECK (points[1].position == doctest::Approx (1.0f));

    // Points move together, no earlier than the song's start, each as far along the travel.
    CHECK (phazite::moveAutomationPoints (context, makeup, { 0, 1 }, BeatDuration::fromBeats (-3.0), -0.2f).inBeats()
           == doctest::Approx (0.0));
    points = phazite::getAutomationPoints (context, makeup);
    CHECK (points[0].position == doctest::Approx (0.5f));
    CHECK (points[1].position == doctest::Approx (0.8f));

    phazite::setAutomationCurve (context, makeup, 0, 0.5f);
    CHECK (phazite::getAutomationPoints (context, makeup)[0].curve == doctest::Approx (0.5f));
    CHECK (song.describeUndo() == juce::String ("Undo Bend Automation"));

    phazite::deleteAutomationPoints (context, makeup, { 2 });
    CHECK (phazite::getAutomationPoints (context, makeup).size() == 2);
    CHECK (song.describeUndo() == juce::String ("Undo Delete Automation Points"));

    // Drawing replaces the points in a range, both its ends included.
    phazite::replaceAutomationPoints (context, makeup, { beats (0), beats (8) }, { { beats (2), 0.2f }, { beats (6), 0.1f } });
    CHECK (beatsOf (phazite::getAutomationPoints (context, makeup)) == std::vector { 2.0, 6.0 });
    CHECK (song.describeUndo() == juce::String ("Undo Draw Automation"));

    phazite::undo (*song.song);
    CHECK (beatsOf (phazite::getAutomationPoints (context, makeup)) == std::vector { 0.0, 8.0 });
    phazite::redo (*song.song);
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
    CHECK (line[0].position == doctest::Approx (0.2f));
    CHECK (line[1].beat == beats (4));
    CHECK (line[1].position == doctest::Approx (0.8f));

    // Four cycles: up at each half beat, down at each beat, and down at the end.
    const auto triangle = phazite::makeAutomationShape (AutomationShape::triangle, range, cycle, 0.0f, 1.0f);
    REQUIRE (triangle.size() == 9);
    CHECK (triangle[1].beat.inBeats() == doctest::Approx (0.5));
    CHECK (triangle[1].position == doctest::Approx (1.0f));
    CHECK (triangle[8].beat == beats (4));
    CHECK (triangle[8].position == doctest::Approx (0.0f));

    // A square jumps: two points at each half beat.
    const auto square = phazite::makeAutomationShape (AutomationShape::square, range, cycle, 0.0f, 1.0f);
    REQUIRE (square.size() == 16);
    CHECK (square[1].beat.inBeats() == doctest::Approx (0.5));
    CHECK (square[2].beat.inBeats() == doctest::Approx (0.5));
    CHECK (square[1].position == doctest::Approx (0.0f));
    CHECK (square[2].position == doctest::Approx (1.0f));

    const auto sine = phazite::makeAutomationShape (AutomationShape::sine, range, cycle, 0.0f, 1.0f);
    REQUIRE (sine.size() == 65);
    CHECK (sine[0].position == doctest::Approx (0.0f));
    CHECK (sine[8].position == doctest::Approx (1.0f)); // half a cycle in

    // A cycle that doesn't fit is stretched to a whole number of them: 4 beats hold 3 of about 1.5.
    const auto saw = phazite::makeAutomationShape (AutomationShape::sawUp, range, BeatDuration::fromBeats (1.5), 0.0f, 1.0f);
    REQUIRE (saw.size() == 6);
    CHECK (saw[1].beat.inBeats() == doctest::Approx (4.0 / 3.0));
    CHECK (saw[1].position == doctest::Approx (1.0f));
    CHECK (saw[2].position == doctest::Approx (0.0f));
}

TEST_CASE ("Automation keeps its beats when the tempo or the time signature changes, in the same undo step")
{
    Song song;
    auto& context = song.context;
    const auto makeup = song.makeup();
    phazite::addAutomationPoint (context, makeup, { beats (4), 0.25f });
    const auto secondsOfPoint = [&song, &context, makeup]
    { return phazite::toSeconds (*song.song, phazite::getAutomationPoints (context, makeup)[0].beat); };
    CHECK (secondsOfPoint() == doctest::Approx (2.0)); // at 120 BPM

    phazite::setTempo (*song.song, 60.0);
    CHECK (phazite::getAutomationPoints (context, makeup)[0].beat.inBeats() == doctest::Approx (4.0));
    CHECK (secondsOfPoint() == doctest::Approx (4.0));

    phazite::undo (*song.song);
    CHECK (phazite::getTempo (*song.song) == doctest::Approx (120.0));
    CHECK (phazite::getAutomationPoints (context, makeup)[0].beat.inBeats() == doctest::Approx (4.0));
    CHECK (secondsOfPoint() == doctest::Approx (2.0));

    phazite::setTimeSignature (*song.song, { 3, 4 });
    CHECK (phazite::getAutomationPoints (context, makeup)[0].beat.inBeats() == doctest::Approx (4.0));
}

TEST_CASE ("Write modes are each track's, the song's, and changing one is no undo step")
{
    Song song;
    auto& context = song.context;
    const auto a = song.a, b = song.b;
    CHECK (phazite::getAutomationMode (context, a) == AutomationMode::read);

    phazite::setAutomationMode (context, a, AutomationMode::touch);
    CHECK (phazite::getAutomationMode (context, a) == AutomationMode::touch);
    CHECK (phazite::getAutomationMode (context, b) == AutomationMode::read);
    CHECK (phazite::findTrackState (*song.song, a)[phazite::schema::trackAutomationMode].toString() == juce::String ("touch"));
    CHECK (phazite::isWritingAutomation (*song.song));
    CHECK_FALSE (song.song->getUndoManager().canUndo());

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
    CHECK_FALSE (phazite::isWritingAutomation (*song.song));
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
    phazite::addEffect (context, song.a, song.find ("phaziteEq"));
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::getLastTouchedParameter (context) == touched);

    // Automate Last Touched shows its lane, and says which it is.
    phazite::CommandRegistry registry;
    phazite::addAutomationCommands (registry, context);
    CHECK (phazite::getCurrentName (*registry.find ("automation.automateLastTouched")) == juce::String ("Automate Makeup"));
    CHECK (registry.find ("automation.automateLastTouched")->overPlugins);
    REQUIRE (registry.invoke ("automation.automateLastTouched"));
    CHECK (lanesOf (context, song.a) == std::vector { touched });

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
    phazite::addAutomationPoint (context, makeup, { beats (0), 0.3f });
    phazite::addAutomationPoint (context, makeup, { beats (2), 0.2f });
    phazite::addAutomationPoint (context, makeup, { beats (4), 0.1f });

    auto& selection = context.getSongSelection();
    selection.selectOnly (song.a);
    selection.selectAutomationPoints (makeup, { 2, 1 });
    CHECK (selection.getTracks().empty()); // points are selected alone
    CHECK (selection.getSelectedAutomationPoints().parameter == makeup);
    CHECK (selection.getSelectedAutomationPoints().indices == std::vector { 1, 2 });
    CHECK (selection.isSelected (makeup, 2));
    CHECK_FALSE (selection.isSelected (makeup, 0));

    phazite::CommandRegistry registry;
    phazite::addSelectionCommands (registry, getSelection (context), getRangeSelection (context), &context);
    phazite::addAutomationCommands (registry, context);
    CHECK (phazite::getCurrentName (*registry.find ("edit.delete")) == juce::String ("Delete 2 Automation Points"));

    // A hundredth of the control's travel.
    REQUIRE (registry.invoke ("automation.raisePoints"));
    auto points = phazite::getAutomationPoints (context, makeup);
    CHECK (points[0].position == doctest::Approx (0.3f));
    CHECK (points[1].position == doctest::Approx (0.21f));
    CHECK (points[2].position == doctest::Approx (0.11f));
    CHECK (song.describeUndo() == juce::String ("Undo Raise Automation Points"));

    // The copies go right after them, the first on the last one's beat, and are selected.
    REQUIRE (registry.invoke ("edit.duplicate"));
    points = phazite::getAutomationPoints (context, makeup);
    CHECK (beatsOf (points) == std::vector { 0.0, 2.0, 4.0, 4.0, 6.0 });
    CHECK (points[3].position == doctest::Approx (0.21f));
    CHECK (selection.getSelectedAutomationPoints().indices == std::vector { 3, 4 });

    REQUIRE (registry.invoke ("edit.delete"));
    CHECK (beatsOf (phazite::getAutomationPoints (context, makeup)) == std::vector { 0.0, 2.0, 4.0 });
    CHECK_FALSE (selection.getSelectedAutomationPoints().parameter.isValid());

    // Selecting anything else deselects them.
    selection.selectAutomationPoints (makeup, { 0 });
    selection.selectOnly (song.b);
    CHECK_FALSE (selection.getSelectedAutomationPoints().parameter.isValid());
    CHECK_FALSE (phazite::CommandRegistry::isEnabled (*registry.find ("automation.raisePoints")));
}

TEST_CASE ("A straight line is an even turn of the knob: a cutoff's halfway is its knob's middle, and keys move along the travel")
{
    // Points are positions along the control's travel, as Ableton Live, Bitwig, Logic and FL Studio keep them (roadmap S4.7):
    // a line from 20 Hz to 20 kHz passes the knob's middle halfway, the geometric middle, not 10 kHz, where a line straight in
    // hertz was halfway; and Up and Down move a point a hundredth of the travel, not a hundredth of 20 kHz.
    Song song;
    auto& context = song.context;
    const auto synth = phazite::getInstrument (context, song.a);
    REQUIRE (synth.has_value());
    const ParameterRef cutoff { synth->id, "cutoff" };
    const auto range = phazite::getParameter (context, cutoff).getRange();
    CHECK (phazite::toControlPosition (range, 20.0f) == doctest::Approx (0.0f));
    CHECK (phazite::toControlPosition (range, 20000.0f) == doctest::Approx (1.0f));
    CHECK (phazite::fromControlPosition (range, 0.5f) == doctest::Approx (std::sqrt (20.0f * 20000.0f)).epsilon (0.001));

    phazite::addAutomationPoint (context, cutoff, { beats (0), phazite::toControlPosition (range, 20.0f) });
    phazite::addAutomationPoint (context, cutoff, { beats (4), phazite::toControlPosition (range, 20000.0f) });
    const auto halfway = phazite::getAutomationPositions (context, cutoff, { beats (2), beats (2) }, 1);
    REQUIRE (halfway.size() == 1);
    CHECK (halfway[0] == doctest::Approx (0.5f));

    // What the engine plays there: the knob's middle, 632 Hz.
    phazite::SongPlayer player (*song.song);
    REQUIRE (player.compile (48000.0, 512, std::nullopt) != nullptr);
    const auto* curve = player.findCurve (cutoff);
    REQUIRE (curve != nullptr);
    CHECK (curve->getValueAt (2.0) == doctest::Approx (632.456f).epsilon (0.001));
    CHECK (curve->getValueAt (0.0) == doctest::Approx (20.0f));
    CHECK (curve->getValueAt (4.0) == doctest::Approx (20000.0f));

    // Raised a hundredth of the travel, the knob's middle goes a hundredth of a turn, not 200 Hz up.
    phazite::replaceAutomationPoints (context, cutoff, { beats (0), beats (8) }, { { beats (0), 0.5f } });
    phazite::CommandRegistry registry;
    phazite::addAutomationCommands (registry, context);
    context.getSongSelection().selectAutomationPoints (cutoff, { 0 });
    REQUIRE (registry.invoke ("automation.raisePoints"));
    const auto raised = phazite::getAutomationPoints (context, cutoff);
    REQUIRE (raised.size() == 1);
    CHECK (raised[0].position == doctest::Approx (0.51f));
    const auto hertz = phazite::fromControlPosition (range, raised[0].position);
    CHECK (hertz > 640.0f);
    CHECK (hertz < 700.0f);

    // Typed in hertz, a value lands where the knob would be, and reads back as typed.
    phazite::setAutomationPointPositions (context, cutoff, { 0 }, phazite::toControlPosition (range, 1000.0f));
    CHECK (phazite::fromControlPosition (range, phazite::getAutomationPoints (context, cutoff)[0].position)
           == doctest::Approx (1000.0f).epsilon (0.0001));
    CHECK (song.describeUndo() == juce::String ("Undo Set Automation Value"));
}

TEST_CASE ("Selected points stay selected, and only they, as points are added or removed before them, undone and redone")
{
    // The selection kept the points' indices: an earlier point's addition undone left it on the points after them, which
    // Delete or a drag then acted on (known bug 53).
    Song song;
    auto& context = song.context;
    const auto makeup = song.makeup();
    phazite::addAutomationPoint (context, makeup, { beats (0), 0.4f });
    phazite::addAutomationPoint (context, makeup, { beats (2), 0.3f });
    phazite::addAutomationPoint (context, makeup, { beats (4), 0.2f });
    phazite::addAutomationPoint (context, makeup, { beats (6), 0.1f });
    phazite::addAutomationPoint (context, makeup, { beats (8), 0.0f });

    auto& selection = context.getSongSelection();
    const auto selectedBeats = [&]
    {
        std::vector<double> result;
        const auto points = phazite::getAutomationPoints (context, makeup);

        for (const auto index : selection.getSelectedAutomationPoints().indices)
            result.push_back (points[(size_t) index].beat.inBeats());

        return result;
    };

    selection.selectAutomationPoints (makeup, { 2, 3 });
    REQUIRE (selectedBeats() == std::vector { 4.0, 6.0 });

    // A point added before them, undone, redone, and another removed before them.
    phazite::addAutomationPoint (context, makeup, { beats (1), 0.35f });
    CHECK (selectedBeats() == std::vector { 4.0, 6.0 });
    phazite::undo (*song.song);
    CHECK (selectedBeats() == std::vector { 4.0, 6.0 });
    phazite::redo (*song.song);
    CHECK (selectedBeats() == std::vector { 4.0, 6.0 });
    phazite::undo (*song.song);
    phazite::undo (*song.song); // the point at 8 gone again: after them
    CHECK (selectedBeats() == std::vector { 4.0, 6.0 });
    phazite::redo (*song.song);

    // Delete takes them, not their neighbours.
    phazite::CommandRegistry registry;
    phazite::addSelectionCommands (registry, getSelection (context), getRangeSelection (context), &context);
    phazite::addAutomationCommands (registry, context);
    phazite::test::UndoCheck undo (*song.song);
    undo.command (registry, "edit.delete");
    CHECK (beatsOf (phazite::getAutomationPoints (context, makeup)) == std::vector { 0.0, 2.0, 8.0 });

    // Undone, the points are back; one that isn't the one selected is never selected in its place.
    phazite::undo (*song.song);
    for (const auto beat : selectedBeats())
        CHECK ((juce::approximatelyEqual (beat, 4.0) || juce::approximatelyEqual (beat, 6.0)));
}

TEST_CASE ("Show Automation collapses the selected tracks' lanes while one shows expanded, and expands them again")
{
    Song song;
    auto& context = song.context;
    const auto a = song.a, b = song.b;
    phazite::addAutomationPoint (context, song.makeup(), { beats (0), 0.4f });

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

TEST_CASE ("Automation is the song's own, in beats, undo and redo too")
{
    Song song;
    auto& document = *song.song;
    const auto makeup = song.makeup();

    // A point goes into the song's curve for the parameter, in beats.
    phazite::addAutomationPoint (song.context, makeup, { beats (4), 0.25f });
    const auto state = phazite::findCurveState (document, makeup);
    REQUIRE (state.isValid());
    CHECK (state.getParent().hasType (phazite::schema::AUTOMATION));
    CHECK (phazite::getCurveParameter (state) == makeup);
    REQUIRE (state.getNumChildren() == 1);
    CHECK ((double) state.getChild (0)[phazite::schema::pointBeat] == doctest::Approx (4.0));
    CHECK ((float) state.getChild (0)[phazite::schema::pointValue] == doctest::Approx (0.25f));

    // An undo takes the song's curve away with its last point, and a redo puts it back.
    phazite::undo (document);
    CHECK_FALSE (phazite::findCurveState (document, makeup).isValid());
    CHECK (phazite::getAutomationPoints (song.context, makeup).empty());
    phazite::redo (document);
    CHECK (phazite::findCurveState (document, makeup).isValid());
    CHECK (phazite::getAutomationPoints (song.context, makeup).size() == 1);
}

TEST_CASE ("A control turned moves its curve's one point, the song's too, in the control's undo step")
{
    // With the song's session playing it.
    Song song;
    auto& context = song.context;
    phazite::addAutomationPoint (context, song.volume(), { beats (2), 0.5f });
    song.song->getUndoManager().clearUndoHistory();

    auto volume = phazite::getVolume (context, song.a);
    volume.beginGesture();
    volume.setValue (0.7f);
    volume.endGesture();

    auto points = phazite::getAutomationPoints (context, song.volume());
    REQUIRE (points.size() == 1);
    CHECK (points[0].beat.inBeats() == doctest::Approx (2.0));
    CHECK (points[0].position == doctest::Approx (0.7f));

    phazite::undo (*song.song);
    points = phazite::getAutomationPoints (context, song.volume());
    REQUIRE (points.size() == 1);
    CHECK (points[0].position == doctest::Approx (0.5f));
}

TEST_CASE ("A duplicated track's automation is its own devices', and the original's stays")
{
    Song song;
    auto& context = song.context;
    phazite::addAutomationPoint (context, song.makeup(), { beats (4), 0.25f });
    phazite::addAutomationPoint (context, song.volume(), { beats (2), 0.25f });

    std::vector<phazite::TrackID> copies;

    {
        const phazite::UndoStep step (*song.song, "Duplicate Track");
        copies = phazite::duplicateTracks (*song.song, { song.a }, nullptr);
    }

    REQUIRE (copies.size() == 1);
    const auto copy = copies[0];
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
        CHECK (copiedPoints[0].position == doctest::Approx (points[0].position));
        CHECK (phazite::findCurveState (*song.song, copied).isValid());
    }

    // The copy's curves go with it in one undo.
    phazite::undo (*song.song);
    CHECK_FALSE (phazite::findCurveState (*song.song, copiedMakeup).isValid());
    CHECK (phazite::getAutomationPoints (context, song.makeup()).size() == 1);
}

TEST_CASE ("A duplicated track's lanes show its own devices")
{
    Song song;
    auto& context = song.context;
    phazite::showAutomationLane (context, song.makeup());
    phazite::showAutomationLane (context, song.volume());

    const auto copies = phazite::duplicateTracks (*song.song, { song.a }, nullptr);
    REQUIRE (copies.size() == 1);
    const auto copy = copies[0];
    const auto copiedEffects = phazite::getEffects (context, copy);
    REQUIRE (copiedEffects.size() == 1);

    CHECK (lanesOf (context, copy)
           == std::vector { ParameterRef { copiedEffects[0].id, "makeup" }, phazite::getVolume (context, copy).getRef() });
    CHECK (lanesOf (context, song.a) == std::vector { song.makeup(), song.volume() });
}

TEST_CASE ("Lanes and the values automation plays are no unsaved changes")
{
    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (temp.folder, "Automated", error);
    REQUIRE (project != nullptr);
    phazite::test::dispatchPendingMessages();

    auto& song = project->getSong();
    const auto track = phazite::getTrackIDs (song)[0];
    const ParameterRef volume { phazite::getFader (song, track), phazite::getVolumeParameterID (song, track) };
    REQUIRE (volume.isValid());

    // Automation is a change; then it is saved.
    phazite::addPoint (song, volume, { phazite::BeatPosition(), 0.5f, 0.0f });
    CHECK (project->hasUnsavedChanges());
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (project->hasUnsavedChanges());

    // The values the automation plays aren't one, nor is a lane.
    {
        phazite::ProjectContext context { phazite::test::getEngineContext() };
        phazite::setSong (context, &song);
        phazite::test::EnginePlayback playback;
        phazite::getSession (context)->play();
        playback.play (0.5);
        phazite::getSession (context)->stop();
        phazite::setSong (context, nullptr);
    }

    phazite::showLane (song, track, volume);
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (project->hasUnsavedChanges());

    // A value set by hand is (Schema.h: channelPan).
    phazite::setChannelPan (song, track, 0.5f);
    CHECK (project->hasUnsavedChanges());
}

TEST_CASE ("A control's menu automates its parameter, and a device's menu lists the device's parameters")
{
    Song song;
    auto& context = song.context;
    const auto a = song.a;

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
    const auto id = phazite::addReturnTrack (*song.song, "Reverb", nullptr);
    REQUIRE (id.isValid());

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

TEST_CASE ("Every edit of automation is one exact undo step, two points at one beat play the later and keep their order, and the walk "
           "back and forth gives each song again")
{
    // Automation.h's edits, each in a step as a command holds it, and AutomationModel.h's, each a step of its own; on A's
    // compressor's makeup, A's volume (a curve of one point) and B's pan; curves copied to a compressor on B and forgotten
    // with it; then the lanes and the write mode, which are no steps.
    Song song;
    auto& context = song.context;
    auto& model = *song.song;
    const auto makeup = song.makeup();
    const auto volume = song.volume();
    const auto pan = phazite::getPan (context, song.b).getRef();
    phazite::test::UndoCheck undo (model);

    const auto inStep = [&undo, &model] (const char* name, const std::function<void()>& edit)
    {
        undo.check (name,
                    [&]
                    {
                        const phazite::UndoStep step (model, name);
                        edit();
                    });
    };

    const auto positionAt = [&model, &makeup] (double beat) { return phazite::getPositions (model, makeup, { beats (beat) })[0]; };

    inStep ("Add Point", [&] { CHECK (phazite::addPoint (model, makeup, { beats (0), 0.5f }) == 0); });
    inStep ("Add Point After", [&] { CHECK (phazite::addPoint (model, makeup, { beats (8), 0.7f }) == 1); });

    // A jump down drawn: two points at beat 4, 0.8 then 0.2 (Schema.h: two at one beat in the order they go), so that an
    // order by value would turn them round. Before the beat the line runs to the first; at the beat, and after, the second
    // plays.
    inStep ("Draw a Jump Down",
            [&]
            {
                phazite::replacePoints (model, makeup, { beats (2), beats (6) },
                                        { { beats (2), 0.5f }, { beats (4), 0.8f }, { beats (4), 0.2f }, { beats (6), 0.5f } });
            });
    REQUIRE (beatsOf (phazite::getPoints (model, makeup)) == std::vector { 0.0, 2.0, 4.0, 4.0, 6.0, 8.0 });
    CHECK (phazite::getPoints (model, makeup)[2].position == doctest::Approx (0.8f));
    CHECK (phazite::getPoints (model, makeup)[3].position == doctest::Approx (0.2f));
    CHECK (positionAt (3.0) == doctest::Approx (0.65f));
    CHECK (positionAt (4.0) == doctest::Approx (0.2f));
    CHECK (positionAt (5.0) == doctest::Approx (0.35f));

    // And a jump up at beat 10, 0.3 then 0.9: the line from beat 8's 0.7 runs down to the first, and the second plays.
    inStep ("Draw a Jump Up",
            [&] { phazite::replacePoints (model, makeup, { beats (9), beats (11) }, { { beats (10), 0.3f }, { beats (10), 0.9f } }); });
    REQUIRE (beatsOf (phazite::getPoints (model, makeup)) == std::vector { 0.0, 2.0, 4.0, 4.0, 6.0, 8.0, 10.0, 10.0 });
    CHECK (phazite::getPoints (model, makeup)[6].position == doctest::Approx (0.3f));
    CHECK (phazite::getPoints (model, makeup)[7].position == doctest::Approx (0.9f));
    CHECK (positionAt (9.0) == doctest::Approx (0.5f));
    CHECK (positionAt (10.0) == doctest::Approx (0.9f));

    // Another point moved, and the first of the jump down deleted: undone, both are back in their order.
    inStep ("Move Point", [&] { phazite::movePoints (model, makeup, { 1 }, BeatDuration::fromBeats (0.5), 0.1f); });
    CHECK (phazite::getPoints (model, makeup)[2].position == doctest::Approx (0.8f));
    CHECK (phazite::getPoints (model, makeup)[3].position == doctest::Approx (0.2f));
    inStep ("Delete Point", [&] { phazite::deletePoints (model, makeup, { 2 }); });
    CHECK (positionAt (4.0) == doctest::Approx (0.2f));
    undo.stepBackOrForth ("Undo Delete Point", [&model] { phazite::undo (model); });
    REQUIRE (beatsOf (phazite::getPoints (model, makeup)) == std::vector { 0.0, 2.5, 4.0, 4.0, 6.0, 8.0, 10.0, 10.0 });
    CHECK (phazite::getPoints (model, makeup)[2].position == doctest::Approx (0.8f));
    CHECK (phazite::getPoints (model, makeup)[3].position == doctest::Approx (0.2f));
    CHECK (positionAt (4.0) == doctest::Approx (0.2f));
    undo.stepBackOrForth ("Redo Delete Point", [&model] { phazite::redo (model); });
    undo.stepBackOrForth ("Undo Delete Point again", [&model] { phazite::undo (model); });

    // The jump up undone (after the point moved since) and redone: its two points back in their order.
    undo.stepBackOrForth ("Undo Move Point", [&model] { phazite::undo (model); });
    undo.stepBackOrForth ("Undo Draw a Jump Up", [&model] { phazite::undo (model); });
    CHECK (beatsOf (phazite::getPoints (model, makeup)) == std::vector { 0.0, 2.0, 4.0, 4.0, 6.0, 8.0 });
    undo.stepBackOrForth ("Redo Draw a Jump Up", [&model] { phazite::redo (model); });
    undo.stepBackOrForth ("Redo Move Point", [&model] { phazite::redo (model); });
    CHECK (phazite::getPoints (model, makeup)[6].position == doctest::Approx (0.3f));
    CHECK (phazite::getPoints (model, makeup)[7].position == doctest::Approx (0.9f));

    inStep ("Bend", [&] { phazite::setCurve (model, makeup, 0, 0.3f); });
    inStep ("Duplicate Points",
            [&] { CHECK (phazite::duplicatePoints (model, makeup, { 2, 3 }, BeatDuration::fromBeats (1.0)).size() == 2); });
    inStep ("Volume's Point", [&] { phazite::addPoint (model, volume, { beats (0), 0.6f }); });
    inStep ("Move the Only Point", [&] { phazite::moveOnlyPoint (model, volume, -6.0f); });
    inStep ("Pan's Points",
            [&]
            {
                phazite::replacePoints (model, pan, { beats (0), beats (16) },
                                        { { beats (0), 0.0f }, { beats (8), 1.0f }, { beats (16), 0.5f } });
            });

    const std::vector<phazite::AutomationStretch> stretch { { song.a, { beats (0), beats (8) } } };
    inStep ("Move Automation", [&] { phazite::moveAutomation (model, stretch, BeatDuration::fromBeats (16.0), false); });
    inStep ("Copy Automation",
            [&] { phazite::moveAutomation (model, { { song.a, { beats (16), beats (24) } } }, BeatDuration::fromBeats (-16.0), true); });
    inStep ("Delete Automation", [&] { phazite::deleteAutomation (model, stretch, false); });
    inStep ("Delete Automation, the Gap Closed",
            [&] { phazite::deleteAutomation (model, { { song.a, { beats (4), beats (8) } } }, true); });
    inStep ("Paste Automation",
            [&]
            {
                const auto copy = phazite::copyAutomation (model, { { song.a, { beats (8), beats (20) } } });
                phazite::pasteAutomation (model, copy, BeatDuration::fromBeats (24.0));
            });
    inStep ("Insert Automation Time", [&] { phazite::insertAutomationTime (model, beats (6), BeatDuration::fromBeats (2.0)); });
    inStep ("Remove Automation Time", [&] { phazite::removeAutomationTime (model, { beats (10), beats (12) }); });
    inStep ("Automation Edges", [&] { phazite::addAutomationEdges (model, { beats (3), beats (9) }); });
    inStep ("Points Taken and Put Back Later",
            [&]
            {
                const auto taken = phazite::getAutomationPointsIn (model, { beats (2), beats (6) });
                phazite::removeAutomationPointsIn (model, { beats (2), beats (6) });
                auto later = taken;

                for (auto& each : later)
                    each.point.beat = each.point.beat + BeatDuration::fromBeats (40.0);

                phazite::addAutomationPoints (model, later);
            });

    // A compressor added to B, and the makeup curve of A's copied to it, as a copy with new IDs gives a copied device its
    // original's automation; then B's compressor removed, its curve kept while an undo could bring it back, and forgotten
    // in a step, as a restore from a damaged commit forgets those a song taken in part brought in.
    undo.check ("Add B's Compressor", [&] { phazite::addEffect (context, song.b, song.find ("phaziteCompressor")); });
    const auto effectsOfB = phazite::getEffects (context, song.b);
    REQUIRE (effectsOfB.size() == 1);
    const ParameterRef makeupOfB { effectsOfB[0].id, "makeup" };
    inStep ("Copy Curves", [&] { phazite::copyCurves (model, { { makeup.device.getRaw(), makeupOfB.device.getRaw() } }); });
    CHECK (beatsOf (phazite::getPoints (model, makeupOfB)) == beatsOf (phazite::getPoints (model, makeup)));
    undo.check ("Remove B's Compressor", [&] { phazite::removeDevice (context, makeupOfB.device); });
    CHECK_FALSE (phazite::getPoints (model, makeupOfB).empty());
    inStep ("Forget Curves of Gone Devices",
            [&] { phazite::forgetCurvesOfGoneDevices (model, &model.getUndoManager(), [] (const juce::ValueTree&) { return true; }); });
    CHECK (phazite::getPoints (model, makeupOfB).empty());
    CHECK_FALSE (phazite::getPoints (model, makeup).empty());

    // As the UI reaches them, each a step of its own; and a lane cleared.
    undo.check ("Add Automation Point", [&] { phazite::addAutomationPoint (context, pan, { beats (2), 0.25f }); });
    undo.check ("Move Automation Points",
                [&] { phazite::moveAutomationPoints (context, pan, { 1 }, BeatDuration::fromBeats (1.0), -0.1f); });
    undo.check ("Bend Automation", [&] { phazite::setAutomationCurve (context, pan, 0, -0.4f); });
    undo.check ("Set Automation Value", [&] { phazite::setAutomationPointPositions (context, pan, { 0, 1 }, 0.75f); });
    undo.check ("Bend Automation of several", [&] { phazite::setAutomationCurves (context, pan, { 0, 1 }, 0.2f); });
    undo.check ("Delete Automation Points", [&] { phazite::deleteAutomationPoints (context, pan, { 0 }); });
    undo.check (
        "Draw Automation",
        [&] { phazite::replaceAutomationPoints (context, pan, { beats (4), beats (12) }, { { beats (5), 0.1f }, { beats (11), 0.9f } }); });
    undo.check ("Clear the Lane", [&] { phazite::deleteAutomation (phazite::getParameter (context, pan)); });
    CHECK (phazite::getPoints (model, pan).empty());
    CHECK (undo.getNumSteps() == 32);

    // The lanes A shows and its write mode, view state and a switch, are no undo steps, and leave what undo steps keep alone
    // (UndoCheck.h); the walk then goes through every step with them as they are left.
    const auto panOfA = phazite::getPan (context, song.a).getRef();
    const auto laneOf = [&model, &song] (const ParameterRef& parameter) -> std::optional<phazite::AutomationLane>
    {
        for (const auto& lane : phazite::getLanes (model, song.a))
            if (lane.parameter == parameter)
                return lane;

        return std::nullopt;
    };

    undo.check ("Show Lane", [&] { phazite::showLane (model, song.a, makeup); });
    REQUIRE (laneOf (makeup).has_value());
    undo.check ("Lane Height", [&] { phazite::setLaneHeight (model, song.a, makeup, 120); });
    CHECK (laneOf (makeup)->height == 120);
    undo.check ("Collapse Lane", [&] { phazite::setLaneCollapsed (model, song.a, makeup, true); });
    CHECK (laneOf (makeup)->collapsed);
    undo.check ("Show Another Lane", [&] { phazite::showLane (model, song.a, volume); });
    undo.check ("Replace Lane", [&] { phazite::replaceLane (model, song.a, volume, panOfA); });
    CHECK (laneOf (panOfA).has_value());
    undo.check ("Remove Lane", [&] { phazite::removeLane (model, song.a, panOfA); });
    CHECK_FALSE (laneOf (panOfA).has_value());
    undo.check ("Latch", [&] { phazite::setAutomationMode (model, song.a, AutomationMode::latch); });
    CHECK (phazite::getAutomationMode (model, song.a) == AutomationMode::latch);
    CHECK (undo.getNumSteps() == 32);

    undo.walkBackAndForth();
    phazite::test::checkSong (model);
}

TEST_CASE ("Two points at one beat keep their order through a save and the project opened again, a jump down and a jump up")
{
    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (temp.folder, "Jumps", error);
    REQUIRE (project != nullptr);
    phazite::test::dispatchPendingMessages();

    auto& song = project->getSong();
    const auto track = phazite::getTrackIDs (song)[0];
    const ParameterRef volume { phazite::getFader (song, track), phazite::getVolumeParameterID (song, track) };
    REQUIRE (volume.isValid());

    // At beat 4, 0.8 then 0.2; at beat 8, 0.3 then 0.9.
    {
        const phazite::UndoStep step (song, "Draw Jumps");
        phazite::replacePoints (
            song, volume, { beats (0), beats (12) },
            { { beats (0), 0.5f }, { beats (4), 0.8f }, { beats (4), 0.2f }, { beats (8), 0.3f }, { beats (8), 0.9f } });
    }

    const auto describe = [] (const std::vector<phazite::AutomationPoint>& points)
    {
        juce::StringArray lines;

        for (const auto& point : points)
            lines.add (juce::String (point.beat.inBeats(), 2) + ": " + juce::String (point.position, 2));

        return lines.joinIntoString ("; ");
    };

    const juce::String drawn ("0.00: 0.50; 4.00: 0.80; 4.00: 0.20; 8.00: 0.30; 8.00: 0.90");
    REQUIRE (describe (phazite::getPoints (song, volume)) == drawn);
    REQUIRE (project->save().wasOk());
    const auto file = project->getFile();
    project.reset();

    const auto reopened = phazite::Project::open (file, false, error);
    REQUIRE (reopened != nullptr);
    CHECK (describe (phazite::getPoints (reopened->getSong(), volume)) == drawn);
}
