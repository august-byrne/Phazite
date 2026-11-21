// SPDX-License-Identifier: AGPL-3.0-or-later
// The piano roll's controller lanes, as the model has them (src/model/notes/NotesModel.h, NoteValues.h: ControlLane): what a
// lane is called, which lanes the clips hold, a lane's values that play, and a draw, a ramp and a delete of them, in clips,
// their aliases and on their channels, each undone exactly.
#include "TestHelpers.h"
#include "TestSong.h"
#include "UndoCheck.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Clips.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"
#include "model/commands/internal/Undo.h"
#include "model/notes/NotesModel.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace
{
using phazite::ControlKind;
using phazite::ControlLane;

phazite::BeatPosition beats (double b)
{
    return phazite::BeatPosition::fromBeats (b);
}

phazite::BeatRange between (double from, double to)
{
    return { beats (from), beats (to) };
}

const ControlLane pedal { ControlKind::controller, 64, 1 };
const ControlLane modWheel { ControlKind::controller, 1, 1 };
const ControlLane bend { ControlKind::pitchWheel, 0, 1 };

// A song with a MIDI clip on its first track from beat 0 to 8, and values put into it as a recording puts them.
struct LaneSong
{
    LaneSong()
    {
        clip = song.addMidiClip (song.firstTrack, 0.0, 8.0);
        getSong().getUndoManager().clearUndoHistory();
    }

    phazite::Song& getSong() const { return *phazite::findSong (song.context); }

    void add (double beat, const ControlLane& lane, int value, int number = -1)
    {
        phazite::addControl (getSong(), clip, beats (beat), lane.kind, number >= 0 ? number : lane.number, value, lane.channel);
    }

    // A lane's values that play, as the piano roll reads them: each as its beat and value.
    std::vector<std::pair<double, int>> valuesOf (const ControlLane& lane, phazite::ClipID of = {}) const
    {
        std::vector<std::pair<double, int>> values;

        for (const auto& point : phazite::getControlValues (song.context, { of.isValid() ? of : clip }, lane))
            values.emplace_back (point.beat.inBeats(), point.value);

        return values;
    }

    // Every value the clip holds of a lane, those that don't play too, by their beats among its notes, in time order.
    std::vector<std::pair<double, int>> heldOf (const ControlLane& lane, phazite::ClipID of = {}) const
    {
        std::vector<std::pair<double, int>> values;

        for (const auto& control : phazite::getClipNotesValue (getSong(), of.isValid() ? of : clip).controls)
            if (control.kind == lane.kind && (lane.kind != ControlKind::controller || control.number == lane.number)
                && control.channel == lane.channel)
                values.emplace_back (control.beat, control.value);

        std::stable_sort (values.begin(), values.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });
        return values;
    }

    static std::vector<phazite::ControlPoint> points (std::initializer_list<std::pair<double, int>> values)
    {
        std::vector<phazite::ControlPoint> made;

        for (const auto& [beat, value] : values)
            made.push_back ({ {}, beats (beat), value, 0 });

        return made;
    }

    phazite::test::TestSong song;
    phazite::ClipID clip;
};

using Values = std::vector<std::pair<double, int>>;
} // namespace

TEST_CASE ("A controller lane is named for what it holds and its channel, and its values are said as the hint bar says them")
{
    CHECK (pedal.getName() == "Sustain Pedal (CC 64)");
    CHECK (pedal.getShortName() == "Sustain Pedal");
    CHECK (modWheel.getName() == "Mod Wheel (CC 1)");
    CHECK (ControlLane { ControlKind::controller, 11, 1 }.getName() == "Expression (CC 11)");
    CHECK (ControlLane { ControlKind::controller, 7, 1 }.getName() == "Volume (CC 7)");
    CHECK (ControlLane { ControlKind::controller, 10, 1 }.getName() == "Pan (CC 10)");
    CHECK (ControlLane { ControlKind::controller, 74, 1 }.getName() == "Brightness (CC 74)");
    CHECK (ControlLane { ControlKind::controller, 21, 1 }.getName() == "CC 21");
    CHECK (ControlLane { ControlKind::controller, 21, 1 }.getShortName() == "CC 21");
    CHECK (bend.getName() == "Pitch Bend");
    CHECK (ControlLane { ControlKind::pitchWheel, 0, 2 }.getName() == "Pitch Bend, Channel 2");
    CHECK (ControlLane { ControlKind::controller, 64, 16 }.getName() == "Sustain Pedal (CC 64), Channel 16");
    CHECK (ControlLane { ControlKind::channelPressure, 0, 1 }.getName() == "Channel Pressure");
    CHECK (ControlLane { ControlKind::aftertouch, 0, 1 }.getName() == "Note Pressure");
    CHECK (ControlLane { ControlKind::programChange, 0, 1 }.getName() == "Program Change");

    CHECK (pedal.describeValue (127) == "Sustain Pedal 127");
    CHECK (bend.describeValue (0) == "Pitch Bend -100%");
    CHECK (bend.describeValue (8192) == "Pitch Bend 0%");
    CHECK (bend.describeValue (16383) == "Pitch Bend 100%");
    CHECK (bend.describeValue (4096) == "Pitch Bend -50%");
    CHECK (ControlLane { ControlKind::programChange, 0, 1 }.describeValue (11) == "Program 12");

    CHECK (bend.getRest() == 8192);
    CHECK (bend.getHighestValue() == 16383);
    CHECK (pedal.getRest() == 0);
    CHECK (pedal.getHighestValue() == 127);
    CHECK_FALSE (ControlLane { ControlKind::aftertouch, 0, 1 }.canDraw());

    // A lane of a kind without a number has none, and every lane is in its ranges.
    CHECK (phazite::makeControlLane (ControlKind::pitchWheel, 12, 3) == ControlLane { ControlKind::pitchWheel, 0, 3 });
    CHECK (phazite::makeControlLane (ControlKind::aftertouch, 60, 1) == ControlLane { ControlKind::aftertouch, 0, 1 });
    CHECK (phazite::makeControlLane (ControlKind::controller, 300, 40) == ControlLane { ControlKind::controller, 127, 16 });

    // The usual ones, on the first channel.
    const auto usual = phazite::getUsualControlLanes();
    REQUIRE (usual.size() == 8);
    CHECK (usual.front() == bend);
    CHECK (std::find (usual.begin(), usual.end(), pedal) != usual.end());
}

TEST_CASE ("The lanes a clip holds are those it plays values of, by channel, then kind, then number, each once")
{
    LaneSong song;
    song.add (1.0, pedal, 127);
    song.add (2.0, pedal, 0);
    song.add (1.5, { ControlKind::pitchWheel, 0, 2 }, 9000);
    song.add (3.0, modWheel, 40);
    song.add (3.0, { ControlKind::aftertouch, 0, 1 }, 50, 60);
    song.add (3.5, { ControlKind::aftertouch, 0, 1 }, 70, 62);
    song.add (4.0, { ControlKind::programChange, 0, 1 }, 5);
    song.add (12.0, { ControlKind::controller, 74, 1 }, 100); // past the clip's end: it doesn't play

    const auto lanes = phazite::getControlLanes (song.song.context, { song.clip });
    const std::vector<ControlLane> expected { modWheel,
                                              pedal,
                                              { ControlKind::aftertouch, 0, 1 },
                                              { ControlKind::programChange, 0, 1 },
                                              { ControlKind::pitchWheel, 0, 2 } };
    CHECK (lanes == expected);

    // A note's pressure, every note's, with each note's number.
    const auto pressure = phazite::getControlValues (song.song.context, { song.clip }, { ControlKind::aftertouch, 0, 1 });
    REQUIRE (pressure.size() == 2);
    CHECK (pressure[0].number == 60);
    CHECK (pressure[1].number == 62);
    CHECK (pressure[1].value == 70);
    CHECK (pressure[0].clip == song.clip);
}

TEST_CASE ("A draw replaces a lane's values in its range, leaving out repeats and values closer than 1/64 of a beat")
{
    LaneSong song;

    for (const auto& [beat, value] : { std::pair { 1.0, 0 }, std::pair { 2.0, 127 }, std::pair { 3.0, 0 }, std::pair { 5.0, 127 } })
        song.add (beat, pedal, value);

    song.add (2.5, modWheel, 90); // another lane's, between: it stays
    phazite::test::UndoCheck undo (song.song.context);

    undo.checkSeveral ("a draw from 1.5 to 3.5",
                       [&]
                       {
                           phazite::setControlValues (
                               song.song.context, song.clip, pedal, between (1.5, 3.5),
                               LaneSong::points ({ { 1.5, 0 }, { 2.0, 64 }, { 2.005, 100 }, { 2.5, 100 }, { 3.0, 20 }, { 4.0, 1 } }));
                       });

    // 1.5 repeats the value before the range, 2.005 takes 2.0's place, 2.5 repeats it, and 4.0 is past the range.
    CHECK (song.valuesOf (pedal) == Values { { 1.0, 0 }, { 2.0, 100 }, { 3.0, 20 }, { 5.0, 127 } });
    CHECK (song.valuesOf (modWheel) == Values { { 2.5, 90 } });

    // A range's ends are in it: a value at its end is replaced too.
    undo.checkSeveral (
        "a draw up to a value",
        [&] { phazite::setControlValues (song.song.context, song.clip, pedal, between (3.0, 5.0), LaneSong::points ({ { 4.0, 64 } })); });
    CHECK (song.valuesOf (pedal) == Values { { 1.0, 0 }, { 2.0, 100 }, { 4.0, 64 } });

    // A click: one value, the others in its range gone.
    undo.checkSeveral (
        "a click",
        [&] { phazite::setControlValues (song.song.context, song.clip, pedal, between (1.0, 2.0), LaneSong::points ({ { 1.0, 127 } })); });
    CHECK (song.valuesOf (pedal) == Values { { 1.0, 127 }, { 4.0, 64 } });
    undo.walkBackAndForth();
}

TEST_CASE ("A ramp is made of a value at each 1/64 of a beat at most, from its start to its end, with no repeats")
{
    const auto spacingIsShortest = [] (const std::vector<phazite::ControlPoint>& ramp)
    {
        for (size_t i = 1; i < ramp.size(); ++i)
            if (ramp[i].beat - ramp[i - 1].beat < phazite::getShortestControlGap() - phazite::BeatDuration::fromBeats (1.0e-9)
                || ramp[i].value == ramp[i - 1].value)
                return false;

        return true;
    };

    // The pedal over a beat, from 0 up to 127: a value at each 1/64 beat, each different.
    const auto up = phazite::makeControlRamp (pedal, beats (1.0), 0, beats (2.0), 127);
    REQUIRE (up.size() >= 2);
    CHECK (up.size() <= 65);
    CHECK (up.front().beat == beats (1.0));
    CHECK (up.front().value == 0);
    CHECK (up.back().beat == beats (2.0));
    CHECK (up.back().value == 127);
    CHECK (spacingIsShortest (up));
    CHECK (std::is_sorted (up.begin(), up.end(), [] (const auto& a, const auto& b) { return a.value < b.value; }));

    // The bend over four beats, its middle down to its lowest: at most one a 1/64 beat, though it has 8,192 values to pass.
    const auto down = phazite::makeControlRamp (bend, beats (0.0), 8192, beats (4.0), 0);
    CHECK (down.size() <= 257);
    CHECK (down.size() > 200);
    CHECK (down.front().value == 8192);
    CHECK (down.back().value == 0);
    CHECK (spacingIsShortest (down));

    // Drawn from right to left, the same ramp; of no length, its end's value; out of range, kept in it.
    const auto back = phazite::makeControlRamp (pedal, beats (2.0), 127, beats (1.0), 0);
    REQUIRE (back.size() == up.size());
    CHECK (back.front().beat == beats (1.0));
    CHECK (back.back().value == 127);

    const auto none = phazite::makeControlRamp (pedal, beats (3.0), 10, beats (3.0), 90);
    REQUIRE (none.size() == 1);
    CHECK (none[0].value == 90);

    const auto flat = phazite::makeControlRamp (pedal, beats (0.0), 200, beats (1.0), 300);
    REQUIRE (flat.size() == 1);
    CHECK (flat[0].value == 127);
}

TEST_CASE ("Only values that play are changed: a trimmed clip's hidden values stay through a draw and a delete across them")
{
    LaneSong song;

    for (const auto beat : { 1.0, 5.0, 6.0, 7.0 })
        song.add (beat, pedal, (int) beat * 10);

    phazite::trimClipEnd (song.song.context, song.clip, beats (5.5));
    phazite::trimClipStart (song.song.context, song.clip, beats (0.5));
    song.add (0.25, pedal, 3); // before the clip's start: hidden
    CHECK (song.valuesOf (pedal) == Values { { 1.0, 10 }, { 5.0, 50 } });
    phazite::test::UndoCheck undo (song.song.context);

    // The clip is drawn across, past both its ends: what it plays changes, and nothing is added where it doesn't play.
    undo.checkSeveral ("a draw across the clip",
                       [&]
                       {
                           phazite::setControlValues (song.song.context, song.clip, pedal, between (0.0, 8.0),
                                                      LaneSong::points ({ { 0.0, 1 }, { 2.0, 64 }, { 6.5, 99 } }));
                       });
    CHECK (song.valuesOf (pedal) == Values { { 2.0, 64 } });
    CHECK (song.heldOf (pedal) == Values { { 0.25, 3 }, { 2.0, 64 }, { 6.0, 60 }, { 7.0, 70 } });

    undo.checkSeveral ("a delete across the clip",
                       [&] { phazite::deleteControlValues (song.song.context, { song.clip }, pedal, between (0.0, 8.0)); });
    CHECK (song.valuesOf (pedal).empty());
    CHECK (song.heldOf (pedal) == Values { { 0.25, 3 }, { 6.0, 60 }, { 7.0, 70 } });
    undo.walkBackAndForth();
}

TEST_CASE ("Every edit of a lane is made in the clip's aliases, and leaves other channels and other controllers alone")
{
    LaneSong song;
    song.add (1.0, pedal, 127);
    song.add (1.0, { ControlKind::controller, 64, 2 }, 127);
    song.add (2.0, modWheel, 30);
    const auto alias = song.song.copyClip (song.clip, 16.0, true);
    CHECK (song.valuesOf (pedal, alias) == Values { { 17.0, 127 } });
    phazite::test::UndoCheck undo (song.song.context);

    // Drawn in the clip, the alias plays it too, where it is.
    undo.checkSeveral (
        "a draw in a linked clip",
        [&] { phazite::setControlValues (song.song.context, song.clip, pedal, between (0.0, 4.0), LaneSong::points ({ { 3.0, 50 } })); });
    CHECK (song.valuesOf (pedal) == Values { { 3.0, 50 } });
    CHECK (song.valuesOf (pedal, alias) == Values { { 19.0, 50 } });

    // Deleted in the alias, from the original too, as one step named for the lane; channel 2's pedal and the mod wheel stay.
    undo.check ("Delete Sustain Pedal", [&] { phazite::deleteAllControlValues (song.song.context, { alias }, pedal); });
    CHECK (phazite::describeUndo (&song.getSong()) == "Undo Delete Sustain Pedal");
    CHECK (song.valuesOf (pedal).empty());
    CHECK (song.valuesOf (pedal, alias).empty());
    CHECK (song.valuesOf ({ ControlKind::controller, 64, 2 }) == Values { { 1.0, 127 } });
    CHECK (song.valuesOf (modWheel) == Values { { 2.0, 30 } });

    // Channel 2's, drawn: channel 1 has none still.
    undo.checkSeveral ("a draw on channel 2",
                       [&]
                       {
                           phazite::setControlValues (song.song.context, song.clip, { ControlKind::controller, 64, 2 }, between (0.0, 8.0),
                                                      LaneSong::points ({ { 4.0, 0 } }));
                       });
    CHECK (song.valuesOf ({ ControlKind::controller, 64, 2 }) == Values { { 4.0, 0 } });
    CHECK (song.valuesOf (pedal).empty());
    undo.walkBackAndForth();
}

TEST_CASE ("A note's pressure is shown and deleted, never drawn")
{
    LaneSong song;
    const ControlLane pressure { ControlKind::aftertouch, 0, 1 };
    song.add (1.0, pressure, 50, 60);
    song.add (1.0, pressure, 80, 64);
    song.add (2.0, pressure, 20, 60);
    phazite::test::UndoCheck undo (song.song.context);

    phazite::setControlValues (song.song.context, song.clip, pressure, between (0.0, 8.0), LaneSong::points ({ { 3.0, 99 } }));
    CHECK (song.valuesOf (pressure) == Values { { 1.0, 50 }, { 1.0, 80 }, { 2.0, 20 } });

    // Deleted from one beat to another, every note's.
    undo.checkSeveral ("Note Pressure deleted",
                       [&] { phazite::deleteControlValues (song.song.context, { song.clip }, pressure, between (0.5, 1.5)); });
    CHECK (song.valuesOf (pressure) == Values { { 2.0, 20 } });
    undo.walkBackAndForth();
}

TEST_CASE ("Values set as they are change nothing: no undo action, no unsaved change, every element where it was")
{
    LaneSong song;
    song.add (1.0, pedal, 127);
    song.add (2.0, pedal, 0);
    song.add (1.5, modWheel, 40);
    song.song.saveWithPluginStates();
    auto& history = song.getSong().getUndoManager();
    history.clearUndoHistory();
    REQUIRE_FALSE (song.song.getProject().hasUnsavedChanges());
    const auto wholeClip = [&song] { return phazite::copyWhole (song.getSong(), phazite::findClipState (song.getSong(), song.clip)); };
    const auto before = wholeClip();

    // A click on the value there, and a draw over a range with the values it holds (a repeat among them, left out as ever).
    {
        phazite::UndoStep step (song.getSong(), "Draw Sustain Pedal");
        phazite::setControlValues (song.song.context, song.clip, pedal, between (1.0, 1.5), LaneSong::points ({ { 1.0, 127 } }));
        phazite::setControlValues (song.song.context, song.clip, pedal, between (0.5, 3.0),
                                   LaneSong::points ({ { 1.0, 127 }, { 1.5, 127 }, { 2.0, 0 }, { 2.5, 0 } }));
        CHECK (history.getNumActionsInCurrentTransaction() == 0);
    }

    CHECK_FALSE (history.canUndo());
    CHECK (wholeClip().isEquivalentTo (before)); // its elements in their order
    CHECK_FALSE (song.song.getProject().hasUnsavedChanges());

    // One value changed among them: that one alone goes and comes, the others staying where they are, in one change of the
    // clip's value (SongValues.h), one action.
    {
        phazite::UndoStep step (song.getSong(), "Draw Sustain Pedal");
        phazite::setControlValues (song.song.context, song.clip, pedal, between (0.5, 3.0),
                                   LaneSong::points ({ { 1.0, 127 }, { 2.0, 10 } }));
        CHECK (history.getNumActionsInCurrentTransaction() == 1);
    }

    CHECK (song.valuesOf (pedal) == Values { { 1.0, 127 }, { 2.0, 10 } });
    CHECK (wholeClip().getChild (0).isEquivalentTo (before.getChild (0)));
}

TEST_CASE ("Delete All of a long take's bend, its mod wheel between, is one change of the clip, and Undo brings every value back")
{
    // A take's 100,000 values, the bend and the mod wheel by turns as a take appends them, among its notes.
    LaneSong song;
    const auto take = song.song.addMidiClip (song.song.firstTrack, 16.0, 800.0);
    std::vector<phazite::ControlValue> many;

    for (int i = 0; i < 100000; ++i)
        many.push_back ({ beats (16.0 + i / 128.0), i % 2 == 0 ? ControlKind::pitchWheel : ControlKind::controller, 1, (i * 37) % 128, 1 });

    for (int i = 0; i < 20; ++i)
        REQUIRE (
            phazite::addNote (song.song.context, take, 60 + i % 12, beats (16.0 + i * 4.0), phazite::BeatDuration::fromBeats (1.0), 100)
                .isValid());

    phazite::addControls (song.getSong(), take, many);
    song.getSong().getUndoManager().clearUndoHistory();

    // What the clip holds but the bend's values, in its order: its whole state's children (WholeState.h), each as text.
    const auto allButTheBend = [&]
    {
        std::vector<juce::String> held;

        for (const auto& child : phazite::copyWhole (song.getSong(), phazite::findClipState (song.getSong(), take)))
            if (! (child.hasType (phazite::schema::CONTROL)
                   && child[phazite::schema::controlKind].toString() == phazite::schema::controlIsPitchWheel))
                held.push_back (child.toXmlString());

        return held;
    };

    const auto kept = allButTheBend();
    REQUIRE (kept.size() == 50020);
    REQUIRE (song.valuesOf (bend, take).size() == 50000);

    struct Heard : juce::ValueTree::Listener
    {
        void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { ++changes; }
        void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { ++changes; }
        void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { ++changes; }
        int changes = 0;
    } heard;

    // The changes of the notes' area, each as the clips it names.
    struct HeardValues : phazite::SongValues::NotesListener
    {
        void clipNotesChanged (const std::vector<phazite::ClipID>& clips) override { changes.push_back (clips); }
        std::vector<std::vector<phazite::ClipID>> changes;
    } heardValues;

    auto* values = song.getSong().getValues();
    REQUIRE (values != nullptr);
    phazite::test::UndoCheck undo (song.song.context);
    auto root = song.getSong().getState();
    root.addListener (&heard);
    values->addNotesListener (&heardValues);
    undo.check ("Delete All Pitch Bend", [&] { phazite::deleteAllControlValues (song.song.context, { take }, bend); });
    values->removeNotesListener (&heardValues);
    root.removeListener (&heard);

    // One change of the clip's value (SongValues.h), which the tree doesn't hear, not 50,000 removals; and the check's undo
    // and redo of it one change each.
    CHECK (heard.changes == 0);
    REQUIRE (heardValues.changes.size() == 3); // the edit, its Undo and its Redo

    for (const auto& clips : heardValues.changes)
        CHECK (clips == std::vector { take });

    CHECK (song.valuesOf (bend, take).empty());
    CHECK (allButTheBend() == kept);
    CHECK (phazite::getClipRange (song.getSong(), take) == between (16.0, 816.0)); // the same clip, by its ID
}

TEST_CASE ("A clip's controllers' values are read at their song beats in its order, and the last of one before a beat is found")
{
    LaneSong s;
    s.add (1.0, pedal, 127);
    s.add (3.0, pedal, 0);
    s.add (2.0, modWheel, 64);
    s.add (3.0, pedal, 90); // a second at one beat, later in the clip's order
    s.add (5.0, ControlLane { ControlKind::controller, 64, 2 }, 30);
    const auto& song = s.getSong();

    const auto values = phazite::getClipControls (song, s.clip);
    REQUIRE (values.size() == 5);
    CHECK (values[0].beat.inBeats() == doctest::Approx (1.0));
    CHECK (values[0].kind == ControlKind::controller);
    CHECK (values[0].number == 64);
    CHECK (values[0].value == 127);
    CHECK (values[2].number == 1);
    CHECK (values[2].value == 64);
    CHECK (values[3].value == 90);
    CHECK (values[4].channel == 2);
    CHECK (phazite::getClipControls (song, {}).empty());

    const auto before = [&song, &s] (double beat, const ControlLane& lane)
    { return phazite::findControlValueBefore (song, s.clip, lane.kind, lane.number, lane.channel, beats (beat)); };

    CHECK_FALSE (before (0.5, pedal).has_value());
    CHECK (before (1.0, pedal) == 127);
    CHECK (before (2.5, pedal) == 127);
    CHECK (before (3.0, pedal) == 90); // the later of the two at beat 3
    CHECK (before (8.0, modWheel) == 64);
    CHECK (before (8.0, ControlLane { ControlKind::controller, 64, 2 }) == 30); // on its own channel
    CHECK (before (8.0, pedal) == 90);
    CHECK_FALSE (before (8.0, bend).has_value());
}
