// SPDX-License-Identifier: AGPL-3.0-or-later
// Automation by IDs and plain values (src/model/automation/AutomationModel.h): the parameters a track offers, the lanes it
// shows as view state, points and their edits as named undo steps, shapes, automation that keeps its beats when the
// tempo changes, the write modes (what they write, on Phazite's engine: EnginePlaybackTests.cpp), the selected points'
// commands, and the parameter touched last.
#include "EnginePlayback.h"
#include "GoldenFiles.h"
#include "SongCheck.h"
#include "TestHelpers.h"
#include "TestTracks.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/SongSelection.h"
#include "model/arrangement/internal/Selection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/CurveValues.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/playback/internal/SongPlayer.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
    CHECK (phazite::test::getTrackProperty (*song.song, a, phazite::schema::trackAutomationMode).toString() == juce::String ("touch"));
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
    CHECK (phazite::getCurveKeys (document) == std::vector { makeup });
    const auto curve = phazite::getCurveValue (document, makeup);
    REQUIRE (curve.has_value());
    REQUIRE (curve->points.size() == 1);
    CHECK (curve->points[0].beat.inBeats() == doctest::Approx (4.0));
    CHECK (curve->points[0].position == doctest::Approx (0.25f));

    // As its file holds it: a curve of the song's automation, by its parameter.
    const auto automation = phazite::test::copyWholeSong (document).getChildWithName (phazite::schema::AUTOMATION);
    REQUIRE (automation.getNumChildren() == 1);
    CHECK (phazite::readCurveParameter (automation.getChild (0)) == makeup);
    CHECK (phazite::readCurveValue (automation.getChild (0)) == *curve);

    // An undo takes the song's curve away with its last point, and a redo puts it back.
    phazite::undo (document);
    CHECK_FALSE (phazite::getCurveValue (document, makeup).has_value());
    CHECK (phazite::getAutomationPoints (song.context, makeup).empty());
    phazite::redo (document);
    CHECK (phazite::getCurveValue (document, makeup).has_value());
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
        CHECK (phazite::getCurveValue (*song.song, copied).has_value());
    }

    // The copy's curves go with it in one undo.
    phazite::undo (*song.song);
    CHECK_FALSE (phazite::getCurveValue (*song.song, copiedMakeup).has_value());
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
    // original's automation; then B's compressor removed, its curve with it in the removal's step, which its Undo brings back
    // and its Redo takes again.
    undo.check ("Add B's Compressor", [&] { phazite::addEffect (context, song.b, song.find ("phaziteCompressor")); });
    const auto effectsOfB = phazite::getEffects (context, song.b);
    REQUIRE (effectsOfB.size() == 1);
    const ParameterRef makeupOfB { effectsOfB[0].id, "makeup" };
    inStep ("Copy Curves", [&] { phazite::copyCurves (model, { { makeup.device.getRaw(), makeupOfB.device.getRaw() } }); });
    CHECK (beatsOf (phazite::getPoints (model, makeupOfB)) == beatsOf (phazite::getPoints (model, makeup)));
    undo.check ("Remove B's Compressor", [&] { phazite::removeDevice (context, makeupOfB.device); });
    CHECK (phazite::getPoints (model, makeupOfB).empty());
    CHECK_FALSE (phazite::getPoints (model, makeup).empty());
    undo.stepBackOrForth ("Undo Remove B's Compressor", [&model] { phazite::undo (model); });
    CHECK (beatsOf (phazite::getPoints (model, makeupOfB)) == beatsOf (phazite::getPoints (model, makeup)));
    undo.stepBackOrForth ("Redo Remove B's Compressor", [&model] { phazite::redo (model); });
    CHECK (phazite::getPoints (model, makeupOfB).empty());

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
    CHECK (undo.getNumSteps() == 31);

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
    CHECK (undo.getNumSteps() == 31);

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

namespace
{
// Which of a curve's points, in its order, hold a key one of its points held before ("k") and which a new one ("n"): an edit
// keeps the key of each point it keeps (moved, bent, or set by its place) and gives each point it makes a new one
// (CurveValues.h: PointValue), as it kept or made the point's element when the song's tree held the curves.
juce::String describeKeys (const std::vector<std::uint64_t>& before, const std::vector<std::uint64_t>& after)
{
    juce::StringArray marks;

    for (const auto key : after)
        marks.add (std::find (before.begin(), before.end(), key) != before.end() ? "k" : "n");

    return marks.joinIntoString (" ");
}

// A curve's element as a whole copy of the song writes one (CurveValues.h: writeCurveElement()), written out by hand.
juce::ValueTree makeCurveElement (const ParameterRef& parameter, const std::vector<phazite::AutomationPoint>& points)
{
    juce::ValueTree curve (phazite::schema::CURVE,
                           { { phazite::schema::curveDevice, juce::String ((juce::int64) parameter.device.getRaw()) },
                             { phazite::schema::curveParameter, parameter.parameter } });

    for (const auto& point : points)
    {
        juce::ValueTree element (phazite::schema::POINT,
                                 { { phazite::schema::pointBeat, point.beat.inBeats() }, { phazite::schema::pointValue, point.position } });

        if (! juce::exactlyEqual (point.curve, 0.0f))
            element.setProperty (phazite::schema::pointCurve, point.curve, nullptr);

        curve.appendChild (element, nullptr);
    }

    return curve;
}

// The song's AUTOMATION as a whole copy of it writes it (WholeState.h) compared with one holding these curves' elements, in
// this order; none for none (a song without curves is written without one).
void checkAutomation (const phazite::Song& song, const std::vector<juce::ValueTree>& curves)
{
    const auto automation = phazite::test::copyWholeSong (song).getChildWithName (phazite::schema::AUTOMATION);

    if (curves.empty())
    {
        CHECK_FALSE (automation.isValid());
        return;
    }

    juce::ValueTree expected (phazite::schema::AUTOMATION);

    for (const auto& curve : curves)
        expected.appendChild (curve.createCopy(), nullptr);

    INFO (automation.toXmlString());
    CHECK (automation.isEquivalentTo (expected));
}

// Points added to a parameter's curve, as the song is built before an edit.
void addPoints (phazite::Song& song, const ParameterRef& parameter, const std::vector<phazite::AutomationPoint>& points)
{
    const phazite::UndoStep step (song, "Points");

    for (const auto& point : points)
        phazite::addPoint (song, parameter, point);
}

// An edit made as one undo step, checked as one (UndoCheck): the actions the step holds (the curves' area's is one for the
// whole step, however often the step set it: ValueArea.h).
int countActionsOf (phazite::test::UndoCheck& undo, phazite::Song& song, const char* name, const std::function<void()>& change)
{
    auto actions = 0;

    undo.check (name,
                [&]
                {
                    const phazite::UndoStep step (song, name);
                    change();
                    actions = song.getUndoManager().getNumActionsInCurrentTransaction();
                });
    return actions;
}

// What the values' listener is told of the curves (SongValues.h: Listener::curvesChanged()): each call's parameters as their
// key texts, sorted, joined by spaces.
struct CurvesTold final : phazite::SongValues::Listener
{
    explicit CurvesTold (const phazite::Song& toHear)
        : song (toHear)
    {
        song.getValues()->addListener (this);
    }

    ~CurvesTold() override { song.getValues()->removeListener (this); }

    void curvesChanged (const std::vector<ParameterRef>& parameters) override
    {
        juce::StringArray texts;

        for (const auto& parameter : parameters)
            texts.add (phazite::getCurveKeyText (parameter));

        texts.sort (false);
        calls.add (texts.joinIntoString (" "));
    }

    // The calls since it was last asked.
    juce::StringArray take()
    {
        const auto taken = calls;
        calls.clear();
        return taken;
    }

    const phazite::Song& song;
    juce::StringArray calls;

    JUCE_DECLARE_NON_COPYABLE (CurvesTold)
};

// A curve's value of these points.
phazite::CurveValue makeCurveValue (const std::vector<phazite::AutomationPoint>& points)
{
    phazite::CurveValue curve;

    for (const auto& point : points)
        curve.points = curve.points.push_back ({ point.beat, point.position, point.curve });

    return curve;
}
} // namespace

TEST_CASE ("Automation's point edits give the song's curves one value for each edit, one undo action, a point kept keeping its "
           "key and a point made getting a new one")
{
    // Roadmap step S5.20: the edits work on the curve's value and give the curves' area the curve they made, once
    // (Automation.cpp: SongCurve::commit()), so that the step holds one action of the area, however many points changed, and
    // each point the edit kept keeps its key, which the selection of points holds, as it kept its element when the tree held
    // the curves. Written out by hand here: the keys kept and made, the step's actions, and the song's curves after.
    Song song;
    auto& model = *song.song;
    const auto makeup = song.makeup();
    const auto volume = song.volume();
    phazite::test::UndoCheck undo (model);
    std::vector<std::uint64_t> keysBefore;
    auto actions = 0;

    const auto edit = [&] (const char* name, const std::function<void()>& change)
    {
        keysBefore = phazite::getPointKeys (model, makeup);
        undo.check (name,
                    [&]
                    {
                        const phazite::UndoStep step (model, name);
                        change();
                        actions = model.getUndoManager().getNumActionsInCurrentTransaction();
                    });
    };

    const auto keys = [&] { return describeKeys (keysBefore, phazite::getPointKeys (model, makeup)); };

    // A point added to a parameter without a curve: the curve, made with its point, a new key.
    edit ("Add a Point", [&] { CHECK (phazite::addPoint (model, makeup, { beats (2), 0.5f, 0.25f }) == 0); });
    CHECK (keys() == "n");
    CHECK (actions == 1);
    checkAutomation (model, { makeCurveElement (makeup, { { beats (2), 0.5f, 0.25f } }) });

    edit ("Add Points",
          [&]
          {
              phazite::addPoint (model, makeup, { beats (0), 0.4f });
              phazite::addPoint (model, makeup, { beats (4), 0.3f });
              phazite::addPoint (model, makeup, { beats (6), 0.2f });
              phazite::addPoint (model, makeup, { beats (8), 0.1f });
          });
    CHECK (keys() == "n k n n n");
    CHECK (actions == 1);

    // A point moved: its key kept.
    auto elements = phazite::getPointKeys (model, makeup);
    edit ("Move a Point", [&] { phazite::movePoints (model, makeup, { 1 }, BeatDuration::fromBeats (0.5), 0.1f); });
    CHECK (actions == 1);
    CHECK (phazite::getPointKeys (model, makeup) == elements);
    checkAutomation (
        model,
        { makeCurveElement (
            makeup,
            { { beats (0), 0.4f }, { beats (2.5), 0.5f + 0.1f, 0.25f }, { beats (4), 0.3f }, { beats (6), 0.2f }, { beats (8), 0.1f } }) });

    // Three dragged later, and dragged back: the same keys, one action each.
    edit ("Drag Three Points", [&] { phazite::movePoints (model, makeup, { 1, 2, 3 }, BeatDuration::fromBeats (1.0), -0.05f); });
    CHECK (actions == 1);
    CHECK (phazite::getPointKeys (model, makeup) == elements);
    edit ("Drag Three Points Back", [&] { phazite::movePoints (model, makeup, { 1, 2, 3 }, BeatDuration::fromBeats (-1.0), 0.0f); });
    CHECK (actions == 1);
    CHECK (phazite::getPointKeys (model, makeup) == elements);
    checkAutomation (model, { makeCurveElement (makeup, { { beats (0), 0.4f },
                                                          { beats (2.5), 0.5f + 0.1f - 0.05f, 0.25f },
                                                          { beats (4), 0.3f - 0.05f },
                                                          { beats (6), 0.2f - 0.05f },
                                                          { beats (8), 0.1f } }) });

    // Bent, and straightened.
    edit ("Bend", [&] { phazite::setCurve (model, makeup, 2, -0.5f); });
    CHECK (keys() == "k k k k k");
    edit ("Straighten", [&] { phazite::setCurve (model, makeup, 1, 0.0f); });
    CHECK (keys() == "k k k k k");
    CHECK (actions == 1);

    // A line drawn over a range: the points in it removed, then the drawn ones added, each after any at its beat, new keys.
    edit ("Draw",
          [&]
          {
              phazite::replacePoints (model, makeup, { beats (1), beats (5) },
                                      { { beats (1.5), 0.9f }, { beats (3), 0.8f }, { beats (4.5), 0.7f } });
          });
    CHECK (keys() == "k n n n k k");
    CHECK (actions == 1);
    checkAutomation (model, { makeCurveElement (makeup, { { beats (0), 0.4f },
                                                          { beats (1.5), 0.9f },
                                                          { beats (3), 0.8f },
                                                          { beats (4.5), 0.7f },
                                                          { beats (6), 0.2f - 0.05f },
                                                          { beats (8), 0.1f } }) });

    // A recording pass's write (playback/AutomationWriter.cpp: replacePoints()): what it passed over replaced by a point
    // every quarter beat, a jump at its start.
    std::vector<phazite::AutomationPoint> pass { { beats (5), 0.15f } };

    for (int i = 0; i <= 8; ++i)
        pass.push_back ({ beats (5.0 + i * 0.25), 0.5f + (float) i * 0.05f });

    edit ("Write a Pass", [&] { phazite::replacePoints (model, makeup, { beats (5), beats (7) }, pass); });
    CHECK (keys() == "k k k k n n n n n n n n n n k");
    CHECK (actions == 1);
    {
        std::vector<phazite::AutomationPoint> written { { beats (0), 0.4f },
                                                        { beats (1.5), 0.9f },
                                                        { beats (3), 0.8f },
                                                        { beats (4.5), 0.7f } };
        written.insert (written.end(), pass.begin(), pass.end());
        written.push_back ({ beats (8), 0.1f });
        checkAutomation (model, { makeCurveElement (makeup, written) });
    }

    // A second curve; then the first emptied and drawn again in one edit: it goes after the other (design J.7), as its
    // element once went and came back, so that the curves' order has it last; its points new.
    edit ("Volume's Point", [&] { phazite::addPoint (model, volume, { beats (0), 0.6f }); });
    CHECK (actions == 1);
    REQUIRE (phazite::getCurveKeys (model) == std::vector { makeup, volume });
    edit ("Draw All Again",
          [&] { phazite::replacePoints (model, makeup, { beats (0), beats (16) }, { { beats (1), 0.3f }, { beats (3), 0.6f } }); });
    CHECK (keys() == "n n");
    CHECK (actions == 1);
    CHECK (phazite::getCurveKeys (model) == std::vector { volume, makeup });
    checkAutomation (model, { makeCurveElement (volume, { { beats (0), 0.6f } }),
                              makeCurveElement (makeup, { { beats (1), 0.3f }, { beats (3), 0.6f } }) });

    // Undone, in its place again.
    undo.stepBackOrForth ("Undo Draw All Again", [&model] { phazite::undo (model); });
    CHECK (phazite::getCurveKeys (model) == std::vector { makeup, volume });
    undo.stepBackOrForth ("Redo Draw All Again", [&model] { phazite::redo (model); });

    // The curve's last point deleted: the curve goes with it.
    edit ("Delete the Only Point", [&] { phazite::deletePoints (model, volume, { 0 }); });
    CHECK (actions == 1);
    CHECK_FALSE (phazite::getCurveValue (model, volume).has_value());
    checkAutomation (model, { makeCurveElement (makeup, { { beats (1), 0.3f }, { beats (3), 0.6f } }) });

    // Edits that change nothing write nothing.
    edit ("Move Nothing", [&] { phazite::movePoints (model, makeup, { 7 }, BeatDuration::fromBeats (1.0), 0.1f); });
    CHECK (actions == 0);

    undo.walkBackAndForth();
    phazite::test::checkSong (model);
}

TEST_CASE ("Selected points stay selected, the same points' keys, through every edit that keeps them")
{
    // The selection of points holds their keys (design S5.20, F): an edit that moves, bends or sets points, and a time edit
    // that moves them, keeps each point's key, so that what was selected is still.
    Song song;
    auto& context = song.context;
    auto& model = *song.song;
    const auto makeup = song.makeup();
    // B's volume, a curve of one point at beat 2: the clips' edit below is A's alone, and the time made at beat 7 goes
    // through every curve but leaves this one as it was (nothing after beat 7 to move, and its line holds its value there,
    // so no point is added), which keeps it a curve of one point for the last edit.
    const auto volume = phazite::getVolume (context, song.b).getRef();
    auto& selection = phazite::getSelection (context);

    for (int i = 0; i < 6; ++i)
        phazite::addPoint (model, makeup, { beats (i * 2.0), 0.2f + (float) i * 0.1f });

    phazite::addPoint (model, volume, { beats (2), 0.5f });
    model.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck undo (model);

    const auto selected = [&]
    {
        const auto points = phazite::getSelectedPoints (selection);
        return points.has_value() ? points->indices : std::vector<int>();
    };

    // The points selected, an edit, and the same keys selected after it, in their curve, wherever it put them.
    const auto keeps =
        [&] (const char* name, const ParameterRef& parameter, const std::vector<int>& indices, const std::function<void()>& change)
    {
        INFO (name);
        phazite::selectPoints (selection, parameter, indices);
        REQUIRE (selected() == indices);
        const auto before = phazite::getPointKeys (model, parameter);
        std::vector<std::uint64_t> chosen;

        for (const auto index : indices)
            chosen.push_back (before[(size_t) index]);

        undo.check (name, change);
        const auto after = phazite::getPointKeys (model, parameter);
        std::vector<int> placesNow;

        for (const auto key : chosen)
        {
            const auto place = std::find (after.begin(), after.end(), key);
            REQUIRE (place != after.end());
            placesNow.push_back ((int) (place - after.begin()));
        }

        CHECK (selected() == placesNow);
        CHECK (selection.getNumObjectsSelected() == (int) indices.size());
        return placesNow;
    };

    const auto inStep = [&model] (const char* name, const std::function<void()>& change)
    {
        return [&model, name, change]
        {
            const phazite::UndoStep step (model, name);
            change();
        };
    };

    CHECK (keeps ("Move", makeup, { 1, 2 },
                  inStep ("Move", [&] { phazite::movePoints (model, makeup, { 1, 2 }, BeatDuration::fromBeats (0.5), 0.05f); }))
           == std::vector { 1, 2 });
    CHECK (keeps ("Bend", makeup, { 3 }, inStep ("Bend", [&] { phazite::setCurve (model, makeup, 3, 0.4f); })) == std::vector { 3 });
    CHECK (keeps ("Set Values", makeup, { 0, 4 }, [&] { phazite::setAutomationPointPositions (context, makeup, { 0, 4 }, 0.75f); })
           == std::vector { 0, 4 });
    CHECK (keeps ("Bend Several", makeup, { 0, 4 }, [&] { phazite::setAutomationCurves (context, makeup, { 0, 4 }, -0.3f); })
           == std::vector { 0, 4 });
    CHECK (keeps ("Shift the Selected", makeup, { 2, 3 },
                  [&] { phazite::moveSelectedAutomationPoints (context, BeatDuration::fromBeats (0.25), 0.1f); })
           == std::vector { 2, 3 });

    // Time made inside the bent line from the point at beat 6: the line cut there, a point added before those selected.
    const auto afterRoom =
        keeps ("Make Room", makeup, { 4, 5 },
               inStep ("Make Room", [&] { phazite::insertAutomationTime (model, beats (7), BeatDuration::fromBeats (4.0)); }));
    CHECK (afterRoom.front() > 4);
    CHECK (phazite::getPoints (model, makeup)[(size_t) afterRoom.front()].beat.inBeats() == doctest::Approx (12.0));

    // Time deleted before them, which moves them back by its length.
    const auto afterDelete =
        keeps ("Delete Time Before Them", makeup, afterRoom,
               inStep ("Delete Time", [&] { phazite::deleteAutomation (model, { { song.a, { beats (1), beats (1.5) } } }, true); }));
    CHECK (phazite::getPoints (model, makeup)[(size_t) afterDelete.front()].beat.inBeats() == doctest::Approx (11.5));

    // The point of a curve of one, moved by its control's value set by hand.
    keeps ("Move the Only Point", volume, { 0 }, inStep ("Move the Only Point", [&] { phazite::moveOnlyPoint (model, volume, -17.0f); }));
    CHECK (phazite::getPoints (model, volume)[0].position != doctest::Approx (0.5f));

    // Undone and redone around them: the same keys, selected again where they are.
    phazite::selectPoints (selection, makeup, { 1, 2 });
    const auto chosenKeys = selection.getPointKeys();
    undo.walkBackAndForth();
    CHECK (selection.getPointKeys() == chosenKeys);
    CHECK (selected() == std::vector { 1, 2 });
    phazite::test::checkSong (model);
}

TEST_CASE ("A point made after an Undo never takes the undone point's key, and a point undone and redone is selected again")
{
    // The song's point keys come from a count its values keep outside the undo history (SongValues.h: givePointKey()), so that
    // no Undo gives a key again: the selection, which still holds the undone point's key, selects no point made after it, and
    // selects the point again once a Redo brings it back with its key.
    Song song;
    auto& model = *song.song;
    const auto makeup = song.makeup();
    auto& selection = phazite::getSelection (song.context);
    phazite::addPoint (model, makeup, { beats (0), 0.2f });
    model.getUndoManager().clearUndoHistory();

    const auto selected = [&selection]
    {
        const auto points = phazite::getSelectedPoints (selection);
        return points.has_value() ? points->indices : std::vector<int>();
    };

    // A made at beat 2 in a step of its own and selected alone: its key.
    const auto makeA = [&]
    {
        {
            const phazite::UndoStep step (model, "Add A");
            phazite::addPoint (model, makeup, { beats (2), 0.4f });
        }

        REQUIRE (phazite::getPoints (model, makeup).size() == 2);
        phazite::selectPoints (selection, makeup, { 1 });
        REQUIRE (selected() == std::vector { 1 });
        return phazite::getPointKeys (model, makeup)[1];
    };

    SUBCASE ("B made after A was undone")
    {
        const auto keyOfA = makeA();
        phazite::undo (model);
        REQUIRE (phazite::getPoints (model, makeup).size() == 1);
        CHECK (selected().empty());

        {
            const phazite::UndoStep step (model, "Add B");
            phazite::addPoint (model, makeup, { beats (3), 0.6f });
        }

        REQUIRE (phazite::getPoints (model, makeup).size() == 2);
        CHECK (phazite::getPointKeys (model, makeup)[1] != keyOfA);
        CHECK (selected().empty());
        CHECK (selection.getNumObjectsSelected() == 0);
    }

    SUBCASE ("A undone and redone")
    {
        const auto keyOfA = makeA();
        phazite::undo (model);
        REQUIRE (phazite::getPoints (model, makeup).size() == 1);
        CHECK (selected().empty());
        phazite::redo (model);
        REQUIRE (phazite::getPoints (model, makeup).size() == 2);
        CHECK (phazite::getPointKeys (model, makeup)[1] == keyOfA);
        CHECK (selected() == std::vector { 1 });
    }

    phazite::test::checkSong (model);
}

TEST_CASE ("Bending several points gives each its bend in the order given, the curve read and set once")
{
    // setAutomationCurves() bends them through one edit of the curve (Automation.cpp: setCurves()), with the same changes as
    // a bend of each alone, one after another: an index the curve hasn't left out; one action of the curves' area.
    Song song;
    auto& model = *song.song;
    const auto makeup = song.makeup();
    addPoints (model, makeup, { { beats (0), 0.2f }, { beats (2), 0.4f, 0.25f }, { beats (4), 0.6f }, { beats (6), 0.8f } });
    model.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck undo (model);
    const auto keys = phazite::getPointKeys (model, makeup);

    CHECK (countActionsOf (undo, model, "Bend Several",
                           [&] { phazite::setAutomationCurves (song.context, makeup, { 3, 0, 3, 1, 7, -1 }, 0.5f); })
           == 1);
    checkAutomation (
        model, { makeCurveElement (
                   makeup, { { beats (0), 0.2f, 0.5f }, { beats (2), 0.4f, 0.5f }, { beats (4), 0.6f }, { beats (6), 0.8f, 0.5f } }) });

    CHECK (
        countActionsOf (undo, model, "Straighten Several", [&] { phazite::setAutomationCurves (song.context, makeup, { 1, 2, 3 }, 0.0f); })
        == 1);
    checkAutomation (
        model, { makeCurveElement (makeup, { { beats (0), 0.2f, 0.5f }, { beats (2), 0.4f }, { beats (4), 0.6f }, { beats (6), 0.8f } }) });
    CHECK (phazite::getPointKeys (model, makeup) == keys);

    undo.walkBackAndForth();
    phazite::test::checkSong (model);
}

TEST_CASE ("Automation's time edits, copies and points added together give their curves the points they always did")
{
    // Roadmap step S5.20: as "Automation's point edits give the song's curves one value for each edit" above, for the
    // writers the clips' edits and the time edits use. Each built on straight lines that hold their values where edges go,
    // so that every point's beat and value is written out here exactly: the song's automation after the edit, as a whole copy
    // writes it, and the one action of the curves' area its step holds.
    Song song;
    auto& model = *song.song;
    const auto makeup = song.makeup();
    const auto volume = song.volume();
    const auto justBefore = 1.0e-4; // where an edge's point on the side before it goes (Automation.cpp: justBefore)

    SUBCASE ("A stretch of a track's automation moved later with its clips")
    {
        addPoints (model, makeup,
                   { { beats (0), 0.2f },
                     { beats (2), 0.2f },
                     { beats (3), 0.8f },
                     { beats (5), 0.8f },
                     { beats (6), 0.2f },
                     { beats (16), 0.2f } });
        addPoints (model, volume, { { beats (0), 0.5f } });
        model.getUndoManager().clearUndoHistory();
        phazite::test::UndoCheck undo (model);

        // Edges at 1, 7 (where it was) and 9, 15 (where it lands), each a point and one just before it; what was in it moved
        // 8 later, what was where it lands gone, then the edges that change nothing gone, the line from 0 straight to where
        // it lands. The volume's flat line keeps its one point.
        CHECK (countActionsOf (
                   undo, model, "Move",
                   [&] { phazite::moveAutomation (model, { { song.a, { beats (1), beats (7) } } }, BeatDuration::fromBeats (8.0), false); })
               == 1);
        checkAutomation (model, { makeCurveElement (makeup, { { beats (0), 0.2f },
                                                              { beats (10), 0.2f },
                                                              { beats (11), 0.8f },
                                                              { beats (13), 0.8f },
                                                              { beats (14), 0.2f },
                                                              { beats (16), 0.2f } }),
                                  makeCurveElement (volume, { { beats (0), 0.5f } }) });
        undo.walkBackAndForth();
    }

    SUBCASE ("Automation pasted where a curve has points")
    {
        addPoints (model, makeup, { { beats (0), 0.2f }, { beats (16), 0.2f } });
        addPoints (model, volume, { { beats (0), 0.5f } });
        model.getUndoManager().clearUndoHistory();
        phazite::test::UndoCheck undo (model);

        // Copied from 2 to 6, pasted 8 later: edges at 10 and 14, what was between them gone, the copy's points from 10.
        const phazite::AutomationCopy copy { makeup,
                                             BeatDuration::fromBeats (4.0),
                                             { { beats (0), 0.9f }, { beats (2), 0.6f } },
                                             beats (2) };
        CHECK (countActionsOf (undo, model, "Paste", [&] { phazite::pasteAutomation (model, { copy }, BeatDuration::fromBeats (8.0)); })
               == 1);
        checkAutomation (model, { makeCurveElement (makeup, { { beats (0), 0.2f },
                                                              { beats (10.0 - justBefore), 0.2f },
                                                              { beats (10), 0.9f },
                                                              { beats (12), 0.6f },
                                                              { beats (14), 0.2f },
                                                              { beats (16), 0.2f } }),
                                  makeCurveElement (volume, { { beats (0), 0.5f } }) });
        undo.walkBackAndForth();
    }

    SUBCASE ("Time removed, with points selected after it")
    {
        addPoints (model, makeup,
                   { { beats (0), 0.2f }, { beats (4), 0.2f }, { beats (5), 0.8f }, { beats (6), 0.4f }, { beats (10), 0.4f } });
        addPoints (model, volume, { { beats (0), 0.5f }, { beats (12), 0.5f } });
        model.getUndoManager().clearUndoHistory();
        phazite::test::UndoCheck undo (model);
        auto& selection = phazite::getSelection (song.context);
        phazite::selectPoints (selection, makeup, { 0, 4 });
        const auto keys = phazite::getPointKeys (model, makeup);

        // From 4 to 6 the points gone, those after it 2 earlier, and a jump at 4 from the value before to the value after;
        // the volume's last point 2 earlier, its value as it was.
        CHECK (countActionsOf (undo, model, "Remove Time", [&] { phazite::removeAutomationTime (model, { beats (4), beats (6) }); }) == 1);
        checkAutomation (
            model, { makeCurveElement (makeup, { { beats (0), 0.2f }, { beats (4), 0.2f }, { beats (4), 0.4f }, { beats (8), 0.4f } }),
                     makeCurveElement (volume, { { beats (0), 0.5f }, { beats (10), 0.5f } }) });

        // The selected points kept their keys, and stay selected.
        const auto selected = phazite::getSelectedPoints (selection);
        REQUIRE (selected.has_value());
        CHECK (selected->indices == std::vector { 0, 3 });
        const auto now = phazite::getPointKeys (model, makeup);
        REQUIRE (now.size() == 4);
        CHECK (now[0] == keys[0]);
        CHECK (now[3] == keys[4]);
        undo.walkBackAndForth();
    }

    SUBCASE ("Edges added to every curve")
    {
        addPoints (model, makeup, { { beats (0), 0.2f }, { beats (4), 0.2f }, { beats (5), 0.6f }, { beats (8), 0.6f } });
        addPoints (model, volume, { { beats (0), 0.5f } });
        model.getUndoManager().clearUndoHistory();
        phazite::test::UndoCheck undo (model);

        // A point at each beat and one just before it, at the line's value there: four on each curve.
        CHECK (countActionsOf (undo, model, "Edges", [&] { phazite::addAutomationEdges (model, { beats (2), beats (6) }); }) == 1);
        checkAutomation (model, { makeCurveElement (makeup, { { beats (0), 0.2f },
                                                              { beats (2.0 - justBefore), 0.2f },
                                                              { beats (2), 0.2f },
                                                              { beats (4), 0.2f },
                                                              { beats (5), 0.6f },
                                                              { beats (6.0 - justBefore), 0.6f },
                                                              { beats (6), 0.6f },
                                                              { beats (8), 0.6f } }),
                                  makeCurveElement (volume, { { beats (0), 0.5f },
                                                              { beats (2.0 - justBefore), 0.5f },
                                                              { beats (2), 0.5f },
                                                              { beats (6.0 - justBefore), 0.5f },
                                                              { beats (6), 0.5f } }) });
        undo.walkBackAndForth();
    }

    SUBCASE ("The points in a range removed, a curve left without any gone")
    {
        addPoints (model, makeup, { { beats (0), 0.2f }, { beats (2), 0.3f }, { beats (4), 0.4f }, { beats (6), 0.5f } });
        addPoints (model, volume, { { beats (3), 0.5f } });
        model.getUndoManager().clearUndoHistory();
        phazite::test::UndoCheck undo (model);

        CHECK (countActionsOf (undo, model, "Remove Points", [&] { phazite::removeAutomationPointsIn (model, { beats (2), beats (6) }); })
               == 1);
        checkAutomation (model, { makeCurveElement (makeup, { { beats (0), 0.2f }, { beats (6), 0.5f } }) });
        undo.walkBackAndForth();
    }

    SUBCASE ("Points added to several curves together, one made")
    {
        addPoints (model, makeup, { { beats (0), 0.2f }, { beats (4), 0.2f } });
        model.getUndoManager().clearUndoHistory();
        phazite::test::UndoCheck undo (model);

        // Each parameter's in the order given, its first point's place among them; two at one beat a jump, in their order.
        const std::vector<phazite::ParameterPoint> points { { makeup, { beats (3), 0.7f } },
                                                            { volume, { beats (1), 0.1f } },
                                                            { makeup, { beats (3), 0.9f } },
                                                            { makeup, { beats (1), 0.4f, 0.3f } } };
        CHECK (countActionsOf (undo, model, "Add Points", [&] { phazite::addAutomationPoints (model, points); }) == 1);
        checkAutomation (
            model, { makeCurveElement (
                         makeup,
                         { { beats (0), 0.2f }, { beats (1), 0.4f, 0.3f }, { beats (3), 0.7f }, { beats (3), 0.9f }, { beats (4), 0.2f } }),
                     makeCurveElement (volume, { { beats (1), 0.1f } }) });
        undo.walkBackAndForth();
    }

    SUBCASE ("Points duplicated over those where they land")
    {
        addPoints (model, makeup,
                   { { beats (0), 0.2f },
                     { beats (2), 0.4f, 0.25f },
                     { beats (3), 0.6f },
                     { beats (4), 0.3f },
                     { beats (5), 0.5f },
                     { beats (8), 0.1f } });
        model.getUndoManager().clearUndoHistory();
        phazite::test::UndoCheck undo (model);
        const auto keys = phazite::getPointKeys (model, makeup);

        // The points from 2 to 3 copied a beat later: the one at 4 replaced, the copy at 3 after the original there, a jump;
        // the copies' keys new.
        std::vector<int> copies;
        CHECK (countActionsOf (undo, model, "Duplicate",
                               [&] { copies = phazite::duplicatePoints (model, makeup, { 2, 1 }, BeatDuration::fromBeats (1.0)); })
               == 1);
        CHECK (copies == std::vector { 3, 4 });
        CHECK (describeKeys (keys, phazite::getPointKeys (model, makeup)) == "k k k n n k k");
        checkAutomation (model, { makeCurveElement (makeup, { { beats (0), 0.2f },
                                                              { beats (2), 0.4f, 0.25f },
                                                              { beats (3), 0.6f },
                                                              { beats (3), 0.4f, 0.25f },
                                                              { beats (4), 0.6f },
                                                              { beats (5), 0.5f },
                                                              { beats (8), 0.1f } }) });
        undo.walkBackAndForth();
    }

    phazite::test::checkSong (model);
}

TEST_CASE ("A whole curve set, taken out, renamed and put in order by its parameter, each one exact undo step, its points by "
           "their places")
{
    // Roadmap step S5.20 (Automation.h: setCurveValue(), eraseCurve(), renameCurve(), setCurveOrder()): what makes, takes out,
    // renames or moves a whole curve, as a version applied, a merge's choice, a copy's curves and a folder made a bus do.
    // Written out by hand: the step's actions, the song's curves after, the parameters the listener is told, and the points
    // selected, whose keys a curve set keeps where they stay.
    Song song;
    auto& model = *song.song;
    const auto makeup = song.makeup();
    const auto volume = song.volume();
    const auto pan = phazite::getPan (song.context, song.a).getRef();
    const auto panOfB = phazite::getPan (song.context, song.b).getRef();
    addPoints (model, makeup, { { beats (0), 0.2f }, { beats (2), 0.4f, 0.25f }, { beats (4), 0.6f } });
    addPoints (model, volume, { { beats (0), 0.5f } });
    model.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck undo (model);
    CurvesTold told (model);
    auto& selection = phazite::getSelection (song.context);

    // A curve the song hasn't: added after the others, in one action, its points given keys of their own.
    CHECK (countActionsOf (
               undo, model, "Set a New Curve",
               [&] { CHECK (phazite::setCurveValue (model, pan, makeCurveValue ({ { beats (1), 0.3f }, { beats (3), 0.7f, -0.5f } }))); })
           == 1);
    checkAutomation (model, { makeCurveElement (makeup, { { beats (0), 0.2f }, { beats (2), 0.4f, 0.25f }, { beats (4), 0.6f } }),
                              makeCurveElement (volume, { { beats (0), 0.5f } }),
                              makeCurveElement (pan, { { beats (1), 0.3f }, { beats (3), 0.7f, -0.5f } }) });
    CHECK (describeKeys ({}, phazite::getPointKeys (model, pan)) == "n n");

    // A curve the song has, by its points' places: each point at a place both have keeps its key, one past the last a new
    // one; the points selected stay selected.
    phazite::selectPoints (selection, makeup, { 0, 2 });
    const auto keys = phazite::getPointKeys (model, makeup);
    CHECK (
        countActionsOf (undo, model, "Set the Curve",
                        [&]
                        {
                            CHECK (phazite::setCurveValue (
                                model, makeup,
                                makeCurveValue ({ { beats (0), 0.2f }, { beats (2), 0.45f }, { beats (4), 0.6f }, { beats (6), 0.9f } })));
                        })
        == 1);
    CHECK (describeKeys (keys, phazite::getPointKeys (model, makeup)) == "k k k n");
    REQUIRE (phazite::getSelectedPoints (selection).has_value());
    CHECK (phazite::getSelectedPoints (selection)->indices == std::vector { 0, 2 });

    // Fewer points: those past the value's last taken out; the selected point still there stays selected.
    CHECK (
        countActionsOf (undo, model, "Set Fewer Points", [&]
                        { CHECK (phazite::setCurveValue (model, makeup, makeCurveValue ({ { beats (0), 0.2f }, { beats (2), 0.45f } }))); })
        == 1);
    REQUIRE (phazite::getSelectedPoints (selection).has_value());
    CHECK (phazite::getSelectedPoints (selection)->indices == std::vector { 0 });
    checkAutomation (model, { makeCurveElement (makeup, { { beats (0), 0.2f }, { beats (2), 0.45f } }),
                              makeCurveElement (volume, { { beats (0), 0.5f } }),
                              makeCurveElement (pan, { { beats (1), 0.3f }, { beats (3), 0.7f, -0.5f } }) });

    // The curve as it is: nothing written, no step.
    const auto steps = undo.getNumSteps();
    told.take();
    CHECK (countActionsOf (undo, model, "Set the Same",
                           [&] { CHECK_FALSE (phazite::setCurveValue (model, makeup, *phazite::getCurveValue (model, makeup))); })
           == 0);
    CHECK (told.take().isEmpty());
    CHECK (undo.getNumSteps() == steps);

    // The curves alike to the song's but for their points' keys (which no file holds): equal, so no change either, whole or
    // a curve alone, no step, the keys held kept.
    {
        const auto heldCurves = phazite::getCurvesValue (model);
        const auto heldKeys = phazite::getPointKeys (model, makeup);
        auto rekeyed = heldCurves;

        for (const auto& parameter : heldCurves.order)
        {
            auto curve = *heldCurves.find (parameter);

            for (size_t i = 0; i < curve.points.size(); ++i)
            {
                auto point = curve.points[i];
                point.key += 1000000;
                curve.points = curve.points.set (i, point);
            }

            rekeyed = phazite::withCurve (rekeyed, parameter, curve);
        }

        REQUIRE (phazite::getPointKeys (model, makeup).size() == 2);
        CHECK (rekeyed.find (makeup)->points[0].key != heldKeys[0]);
        CHECK_FALSE (rekeyed.isSameAs (heldCurves));
        CHECK (rekeyed == heldCurves);
        CHECK (*rekeyed.find (makeup) == *heldCurves.find (makeup));
        CHECK (countActionsOf (undo, model, "Set the Same Keyed Otherwise", [&] { model.getValues()->setCurves (model, rekeyed); }) == 0);
        CHECK (countActionsOf (undo, model, "Set a Curve the Same Keyed Otherwise",
                               [&] { CHECK_FALSE (phazite::setCurveValue (model, makeup, *rekeyed.find (makeup))); })
               == 0);
        CHECK (phazite::getCurvesValue (model).isSameAs (heldCurves));
        CHECK (phazite::getPointKeys (model, makeup) == heldKeys);
        CHECK (told.take().isEmpty());
        CHECK (undo.getNumSteps() == steps);
    }

    // Renamed: in its place, in one action, and what hears the curves change told the parameter it had and the one it has at
    // once, in its Undo and its Redo too (no parameter in between); its points' keys kept.
    const auto volumeKeys = phazite::getPointKeys (model, volume);
    CHECK (countActionsOf (undo, model, "Rename", [&] { phazite::renameCurve (model, volume, panOfB); }) == 1);
    juce::StringArray both { phazite::getCurveKeyText (volume), phazite::getCurveKeyText (panOfB) };
    both.sort (false);
    const auto once = both.joinIntoString (" ");
    CHECK (told.take() == juce::StringArray { once, once, once }); // the step, its Undo, its Redo
    CHECK (phazite::getCurveKeys (model) == std::vector { makeup, panOfB, pan });
    CHECK (phazite::getPointKeys (model, panOfB) == volumeKeys);

    // A parameter without a curve: nothing.
    CHECK (countActionsOf (undo, model, "Rename None", [&] { phazite::renameCurve (model, volume, pan); }) == 0);

    // Put in another order: those named first, in their order, the others after them; what hears the curves change told
    // those whose places changed.
    told.take();
    CHECK (countActionsOf (undo, model, "Order", [&] { CHECK (phazite::setCurveOrder (model, { pan, makeup })); }) == 1);
    CHECK (phazite::getCurveKeys (model) == std::vector { pan, makeup, panOfB });
    const auto textOf = [] (std::initializer_list<ParameterRef> parameters)
    {
        juce::StringArray texts;

        for (const auto& parameter : parameters)
            texts.add (phazite::getCurveKeyText (parameter));

        texts.sort (false);
        return texts.joinIntoString (" ");
    };
    const auto allThree = textOf ({ makeup, panOfB, pan });
    CHECK (told.take() == juce::StringArray { allThree, allThree, allThree }); // the step, its Undo, its Redo: each moved

    // The first two swapped: those two told, the third, in its place, not.
    CHECK (countActionsOf (undo, model, "Swap the First Two", [&] { CHECK (phazite::setCurveOrder (model, { makeup, pan })); }) == 1);
    CHECK (phazite::getCurveKeys (model) == std::vector { makeup, pan, panOfB });
    const auto firstTwo = textOf ({ makeup, pan });
    CHECK (told.take() == juce::StringArray { firstTwo, firstTwo, firstTwo });
    CHECK (countActionsOf (undo, model, "Same Order", [&] { CHECK_FALSE (phazite::setCurveOrder (model, { makeup, pan, panOfB })); }) == 0);
    CHECK (told.take().isEmpty());

    // Taken out: one; one that isn't there, nothing.
    CHECK (countActionsOf (undo, model, "Erase", [&] { CHECK (phazite::eraseCurve (model, panOfB)); }) == 1);
    CHECK (countActionsOf (undo, model, "Erase None", [&] { CHECK_FALSE (phazite::eraseCurve (model, panOfB)); }) == 0);

    // The others taken out in one step, one action: the song without curves, written without an AUTOMATION, its tree keeping
    // the empty place; what hears the curves change told of each, and of both at once for its Undo and its Redo.
    told.take();
    CHECK (countActionsOf (undo, model, "Erase All",
                           [&]
                           {
                               CHECK (phazite::eraseCurve (model, pan));
                               CHECK (phazite::eraseCurve (model, makeup));
                           })
           == 1);
    // The step tells each curve as it is erased (the area tells once for each change of its value: SongValues.h,
    // Listener::curvesChanged()); its Undo and its Redo, each one change of the value, tell both at once.
    const auto theTwoLeft = textOf ({ pan, makeup });
    CHECK (told.take() == juce::StringArray { textOf ({ pan }), textOf ({ makeup }), theTwoLeft, theTwoLeft });
    checkAutomation (model, {});
    CHECK (model.getState().getChildWithName (phazite::schema::AUTOMATION).isValid());

    // A curve set into a song without curves: one action.
    CHECK (countActionsOf (undo, model, "Set into None",
                           [&] { CHECK (phazite::setCurveValue (model, volume, makeCurveValue ({ { beats (0), 0.5f } }))); })
           == 1);
    checkAutomation (model, { makeCurveElement (volume, { { beats (0), 0.5f } }) });

    undo.walkBackAndForth();
    phazite::test::checkSong (model);
}

TEST_CASE ("A duplicated track and a duplicated chain give their copies their originals' curves after the song's others, in one "
           "exact undo step")
{
    // Automation.h: copyCurves(), through setCurveValue(): each copied device's curves, in the order of the originals', after the
    // song's other curves, each written as its original is, its points given keys of their own; a curve of what isn't copied
    // left alone.
    Song song;
    auto& model = *song.song;
    auto& context = song.context;
    const auto makeup = song.makeup();
    const auto volume = song.volume();
    const auto volumeOfB = phazite::getVolume (context, song.b).getRef();
    addPoints (model, makeup, { { beats (0), 0.2f }, { beats (2), 0.4f, 0.25f } });
    addPoints (model, volume, { { beats (1), 0.5f } });
    addPoints (model, volumeOfB, { { beats (3), 0.7f } });
    model.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck undo (model);

    std::vector<phazite::TrackID> copies;
    CHECK (countActionsOf (undo, model, "Duplicate Track", [&] { copies = phazite::duplicateTracks (model, { song.a }, nullptr); }) > 0);
    REQUIRE (copies.size() == 1);
    const auto copiedEffects = phazite::getEffects (context, copies[0]);
    REQUIRE (copiedEffects.size() == 1);
    const ParameterRef copiedMakeup { copiedEffects[0].id, "makeup" };
    const auto copiedVolume = phazite::getVolume (context, copies[0]).getRef();
    checkAutomation (model, { makeCurveElement (makeup, { { beats (0), 0.2f }, { beats (2), 0.4f, 0.25f } }),
                              makeCurveElement (volume, { { beats (1), 0.5f } }), makeCurveElement (volumeOfB, { { beats (3), 0.7f } }),
                              makeCurveElement (copiedMakeup, { { beats (0), 0.2f }, { beats (2), 0.4f, 0.25f } }),
                              makeCurveElement (copiedVolume, { { beats (1), 0.5f } }) });
    CHECK (describeKeys (phazite::getPointKeys (model, makeup), phazite::getPointKeys (model, copiedMakeup)) == "n n");

    // A's compressor grouped into a container, its chain's volume automated, then the chain duplicated: the copied
    // compressor's and the copied chain's curves.
    phazite::DeviceID container;
    countActionsOf (undo, model, "Group", [&] { container = phazite::groupIntoContainer (model, { makeup.device }); });
    REQUIRE (container.isValid());
    const auto chains = phazite::getChains (model, container);
    REQUIRE (chains.size() == 1);
    const ParameterRef chainVolume { chains[0], phazite::schema::chainVolumeParameter };
    countActionsOf (undo, model, "Chain Volume", [&] { phazite::addPoint (model, chainVolume, { beats (2), 0.6f }); });

    phazite::DeviceID copy;
    CHECK (countActionsOf (undo, model, "Duplicate Chain", [&] { copy = phazite::duplicateChain (model, chains[0]); }) > 0);
    REQUIRE (copy.isValid());
    const auto inCopy = phazite::getChainDevices (model, copy);
    REQUIRE (inCopy.size() == 1);
    const ParameterRef makeupInCopy { inCopy[0], "makeup" }, copiedChainVolume { copy, chainVolume.parameter };
    CHECK (phazite::getCurveKeys (model)
           == std::vector { makeup, volume, volumeOfB, copiedMakeup, copiedVolume, chainVolume, makeupInCopy, copiedChainVolume });
    CHECK (phazite::getCurveValue (model, makeupInCopy) == phazite::getCurveValue (model, makeup));
    CHECK (phazite::getCurveValue (model, copiedChainVolume) == phazite::getCurveValue (model, chainVolume));

    undo.walkBackAndForth();
    phazite::test::checkSong (model);
}
