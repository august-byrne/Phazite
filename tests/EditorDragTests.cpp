// SPDX-License-Identifier: AGPL-3.0-or-later
// The editors' drags while the song changes under them, and what ends them: between a press and its release, Ctrl+Z and a
// command's key (which run nothing then: ui/input/KeyDispatcher.h), the item dragged deleted through the model, and another
// project opened; Escape, which takes a drag back in every editor (TimelineEditor::keyPressed()); and the copies of a Ctrl+drag
// that comes back to its start. Each ends with the song as the model says, every undo step exact (UndoCheck.h). And the
// wheel, turned by the parts of a notch a trackpad sends, over each place it acts; and automation and tempo points off the
// grid, which keep their offset from it as they are dragged.
#include "AppScreen.h"
#include "EditorScreens.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/transport/TempoModel.h"
#include "model/transport/TransportModel.h"
#include "ui/audioclip/AudioClipEditor.h"
#include "ui/components/Controls.h"
#include "ui/theme/DesignTokens.h"

namespace
{
using phazite::BeatPosition;
using phazite::test::beatAt;
using phazite::test::getKey;
using phazite::test::Mouse;
using phazite::test::PianoRollScreen;
using phazite::test::PlaylistScreen;
using phazite::test::SongPart;
using phazite::test::UndoCheck;
using phazite::test::writeSong;
using phazite::ui::Tool;

const auto ctrl = juce::ModifierKeys (juce::ModifierKeys::commandModifier);
const auto escape = juce::KeyPress (juce::KeyPress::escapeKey);

// Keys pressed and let go, as the keymap describes them.
void pressKeys (phazite::test::TestWindow& window, std::initializer_list<const char*> keys)
{
    for (const auto* key : keys)
    {
        window.keyGoesDown();
        window.pressKey (getKey (key));
        window.releaseKeys();
    }
}

// The keys that change the song, pressed and let go in the middle of a drag: Undo, Redo, and Delete, which would delete what
// is dragged (it is selected). None runs while the mouse is down.
void pressSongKeys (phazite::test::TestWindow& window)
{
    pressKeys (window, { "command + Z", "command + shift + Z", "delete" });
}

// The playlist with the volume's automation lane of its first track shown, and the tempo's lane, both with the model's
// points on them.
struct LanesScreen : PlaylistScreen
{
    LanesScreen()
    {
        volume = phazite::getVolume (context, tracks[0]).getRef();
        phazite::showAutomationLane (phazite::getVolume (context, tracks[0]));
        phazite::addTempoChange (context, beatAt (8.0), 140.0);
        getSong().getUndoManager().clearUndoHistory();
        phazite::test::dispatchPendingMessages();

        lane = phazite::test::findComponentWithTitle (playlist, "Automation of Volume");
        tempoLane = dynamic_cast<phazite::ui::TempoLane*> (phazite::test::findComponentWithTitle (playlist, "Tempo"));
        REQUIRE (lane != nullptr);
        REQUIRE (tempoLane != nullptr);
        REQUIRE (tempoLane->isVisible());
    }

    // Where a beat and a value are in the volume's lane, values from 0 to 1, as the lane draws them: in the playlist, which a
    // test's mouse uses, as the lane goes when another project opens.
    juce::Point<int> inLane (double beat, float value) const
    {
        const auto area =
            lane->getLocalBounds().toFloat().withTrimmedBottom (1.0f).reduced (0.0f, phazite::ui::LaneTint::selectedPointRadius + 1.0f);
        return playlist.getLocalPoint (lane, juce::Point<int> (xOf (beat), juce::roundToInt (area.getBottom() - value * area.getHeight())));
    }

    std::vector<phazite::AutomationPoint> getPoints() const { return phazite::getAutomationPoints (context, volume); }
    std::vector<phazite::TempoChange> getTempos() const { return phazite::getTempoChanges (context); }
    juce::Point<int> tempoPoint (int index) const { return tempoLane->getPointPosition (index).roundToInt(); }

    phazite::ParameterRef volume;
    juce::Component* lane = nullptr;
    phazite::ui::TempoLane* tempoLane = nullptr;
};

// An audio clip editor in a window, showing a sine of 4 seconds from beat 4 to 12, with the keys and commands the app gives it.
struct AudioScreen
{
    AudioScreen()
    {
        track = phazite::ensureTracks (*song, 1)[0];
        setSong (context, song.get());
        const auto file = phazite::test::writeSound (temp.folder.getChildFile ("Vocal.wav"), 4.0);
        const auto imported = phazite::importFiles (context, { file.getFullPathName() }, track, beatAt (4.0));
        REQUIRE (imported.size() == 1);
        clip = imported[0];
        song->getUndoManager().clearUndoHistory();

        phazite::addUndoCommands (registry, phazite::getSelection (context));
        phazite::addSelectionCommands (registry, phazite::getSelection (context), phazite::getRangeSelection (context));
        phazite::ui::addTimelineCommands (registry, [this] { return &editor; });
        phazite::test::deliverChanges();
        window.addKeyListener (&keys);
        editor.setGrid (phazite::test::getBeatGrid());
        editor.showClip (clip);
        phazite::test::dispatchPendingMessages();
    }

    ~AudioScreen()
    {
        window.removeKeyListener (&keys);
        setSong (context, nullptr);
    }

    phazite::AudioClipInfo get() const
    {
        const auto info = phazite::getAudioClip (context, clip);
        REQUIRE (info.has_value());
        return *info;
    }

    juce::Component& waveform() { return editor.getWaveformComponent(); }
    int xOf (double beat) { return juce::roundToInt (editor.getView().beatToX (beatAt (beat))); }
    juce::Point<int> trimStartGrip() { return editor.getHandleBounds (phazite::ui::AudioClipEditor::Handle::trimStart).getCentre(); }

    phazite::test::TemporaryFolder temp;
    phazite::Song::Ptr song = phazite::createSong(); // 120 BPM, 4/4
    phazite::TrackID track;
    phazite::ClipID clip;
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::KeyDispatcher keys { commands };
    phazite::ui::AudioClipEditor editor { commands, context };
    phazite::test::TestWindow window { editor, 1000, 400 };
};

// How much of a notch each of a trackpad's moves is, here: a part that adds up to whole notches exactly.
float partOfNotch (int parts)
{
    return (float) phazite::ui::wheelNotch / (float) parts;
}
} // namespace

//==============================================================================
TEST_CASE ("While a clip is dragged, Ctrl+Z, Redo and Delete run nothing, and the drag is one undo step; after it, Undo undoes it")
{
    PlaylistScreen screen;
    const auto clip = screen.addMidiClip (0, 0.0, 2.0), other = screen.addMidiClip (1, 0.0, 2.0);
    phazite::test::dispatchPendingMessages();
    screen.getSong().getUndoManager().clearUndoHistory();
    Mouse mouse (*screen.lanes);
    UndoCheck undo (screen.getSong());

    // A step before the drag, which an Undo in the drag would have taken back before the drag's first change.
    undo.check ("a move",
                [&]
                {
                    mouse.press (screen.lower (1.0, 1));
                    mouse.dragTo (screen.lower (2.0, 1));
                    mouse.dragTo (screen.lower (3.0, 1));
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    // Known bug 45 until 2026-11-14: Ctrl+Z before the drag's first change undid the step before, and in the middle of it the
    // drag's own step, the rest of the drag going into a step without a name.
    undo.check ("a move with Ctrl+Z pressed in it",
                [&]
                {
                    mouse.press (screen.lower (1.0, 0));
                    pressKeys (screen.window, { "command + Z" });
                    mouse.dragTo (screen.lower (2.0, 0));
                    mouse.dragTo (screen.lower (3.0, 0));
                    pressKeys (screen.window, { "command + Z" });
                    mouse.dragTo (screen.lower (3.0, 0).translated (1, 0));
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK (screen.get (clip).range.getStart().inBeats() == doctest::Approx (2.0));
    CHECK (screen.get (other).range.getStart().inBeats() == doctest::Approx (2.0));
    CHECK (screen.describeUndo() == "Undo Move Clip");

    undo.check ("a move with Ctrl+Z, Redo and Delete pressed in it",
                [&]
                {
                    mouse.press (screen.lower (3.0, 0));
                    pressSongKeys (screen.window);
                    mouse.dragTo (screen.lower (4.0, 0));
                    mouse.dragTo (screen.lower (5.0, 0));
                    pressSongKeys (screen.window);
                    mouse.dragTo (screen.lower (5.0, 0).translated (1, 0));
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK (screen.has (clip));
    CHECK (screen.get (clip).range.getStart().inBeats() == doctest::Approx (4.0));
    CHECK (screen.get (other).range.getStart().inBeats() == doctest::Approx (2.0));
    CHECK (screen.describeUndo() == "Undo Move Clip");
    CHECK (undo.getNumSteps() == 3);

    // With the mouse up, the keys are the commands' again.
    undo.stepBackOrForth ("Ctrl+Z", [&] { screen.window.pressKey (getKey ("command + Z")); });
    phazite::test::deliverChanges();
    screen.window.releaseKeys();
    CHECK (screen.get (clip).range.getStart().inBeats() == doctest::Approx (2.0));
    undo.walkBackAndForth();
}

TEST_CASE ("While a note, an automation point, a tempo change, a velocity or an audio clip's edge is dragged, Ctrl+Z, Redo and "
           "Delete run nothing")
{
    SUBCASE ("A note")
    {
        PianoRollScreen screen;
        const auto note = screen.addNote (1.0);
        screen.getSong().getUndoManager().clearUndoHistory();
        Mouse mouse (*screen.notes);
        UndoCheck undo (screen.getSong());

        undo.check ("a note's move with the keys pressed in it",
                    [&]
                    {
                        mouse.press (screen.middleOf (note));
                        mouse.dragTo (screen.middleOf (note).translated (screen.widthOf (1.0), 0));
                        pressSongKeys (screen.window);
                        mouse.dragTo (screen.middleOf (note).translated (screen.widthOf (1.0), 0));
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();

        CHECK (screen.has (note));
        CHECK (screen.get (note).start.inBeats() == doctest::Approx (3.0));
        CHECK (screen.describeUndo() == "Undo Move Notes");
        undo.walkBackAndForth();
    }

    SUBCASE ("A velocity")
    {
        PianoRollScreen screen;
        const auto note = screen.addNote (1.0);
        screen.getSong().getUndoManager().clearUndoHistory();
        Mouse mouse (*screen.velocity);
        UndoCheck undo (screen.getSong());
        const auto x = screen.xOf (1.0);

        undo.check ("a velocity drawn with the keys pressed in it",
                    [&]
                    {
                        mouse.press ({ x, 10 });
                        phazite::test::deliverChanges();
                        pressSongKeys (screen.window);
                        mouse.dragTo ({ x, screen.velocity->getHeight() - 3 });
                        phazite::test::deliverChanges();
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();

        CHECK (screen.has (note));
        CHECK (screen.get (note).velocity < 20);
        CHECK (screen.describeUndo() == "Undo Change Velocity");
        undo.walkBackAndForth();
    }

    SUBCASE ("An automation point, and a tempo change")
    {
        LanesScreen screen;
        phazite::addAutomationPoint (screen.context, screen.volume, { beatAt (4.0), 0.5f });
        phazite::test::deliverChanges();
        screen.getSong().getUndoManager().clearUndoHistory();
        phazite::test::dispatchPendingMessages();
        UndoCheck undo (screen.getSong());

        Mouse laneMouse (screen.playlist);
        undo.check ("a point's move with the keys pressed in it",
                    [&]
                    {
                        laneMouse.press (screen.inLane (4.0, 0.5f));
                        laneMouse.dragTo (screen.inLane (5.0, 0.5f));
                        pressSongKeys (screen.window);
                        laneMouse.dragTo (screen.inLane (6.0, 0.5f));
                        laneMouse.release();
                        laneMouse.pause();
                    });
        phazite::test::deliverChanges();

        REQUIRE (screen.getPoints().size() == 1);
        CHECK (screen.getPoints()[0].beat.inBeats() == doctest::Approx (6.0));
        CHECK (screen.describeUndo() == "Undo Move Automation Points");

        Mouse tempoMouse (*screen.tempoLane);
        undo.check ("a tempo change's move with the keys pressed in it",
                    [&]
                    {
                        const auto from = screen.tempoPoint (1);
                        tempoMouse.press (from);
                        tempoMouse.dragTo (from.translated (screen.widthOf (1.0), 0));
                        pressSongKeys (screen.window);
                        tempoMouse.dragTo (from.translated (screen.widthOf (2.0), 0));
                        tempoMouse.release();
                        tempoMouse.pause();
                    });
        phazite::test::deliverChanges();

        REQUIRE (screen.getTempos().size() == 2);
        CHECK (screen.getTempos()[1].beat.inBeats() == doctest::Approx (10.0));
        CHECK (screen.describeUndo() == "Undo Move Tempo Change");
        undo.walkBackAndForth();
    }

    SUBCASE ("An audio clip's start, in the audio clip editor")
    {
        AudioScreen screen;
        Mouse mouse (screen.waveform());
        UndoCheck undo (*screen.song);
        const auto middleY = screen.waveform().getHeight() / 2;

        undo.check ("a trim with the keys pressed in it",
                    [&]
                    {
                        mouse.press (screen.trimStartGrip());
                        mouse.dragTo ({ screen.xOf (4.6), middleY });
                        phazite::test::deliverChanges();
                        pressSongKeys (screen.window);
                        mouse.dragTo ({ screen.xOf (5.1), middleY });
                        phazite::test::deliverChanges();
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();

        CHECK (screen.get().clip.range.getStart().inBeats() == doctest::Approx (5.0));
        CHECK (phazite::describeUndo (screen.song.get()) == "Undo Trim Clip Start");
        undo.walkBackAndForth();
    }
}

//==============================================================================
TEST_CASE ("What a drag holds, deleted through the model in the middle of it, leaves the song as the model has it, in one step")
{
    SUBCASE ("A clip")
    {
        PlaylistScreen screen;
        const auto clip = screen.addMidiClip (0, 0.0, 2.0);
        phazite::test::dispatchPendingMessages();
        screen.getSong().getUndoManager().clearUndoHistory();
        Mouse mouse (*screen.lanes);
        UndoCheck undo (screen.getSong());

        undo.check ("a move whose clip goes",
                    [&]
                    {
                        mouse.press (screen.lower (1.0, 0));
                        mouse.dragTo (screen.lower (3.0, 0));
                        phazite::deleteClips (screen.context, { clip }, false);
                        phazite::test::dispatchPendingMessages();
                        mouse.dragTo (screen.lower (5.0, 1));
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();

        CHECK_FALSE (screen.has (clip));
        CHECK (phazite::getClips (screen.context, screen.tracks[1]).empty());
        undo.walkBackAndForth();
    }

    SUBCASE ("A note")
    {
        PianoRollScreen screen;
        const auto note = screen.addNote (1.0);
        screen.getSong().getUndoManager().clearUndoHistory();
        Mouse mouse (*screen.notes);
        UndoCheck undo (screen.getSong());

        undo.check ("a move whose note goes",
                    [&]
                    {
                        const auto from = screen.middleOf (note);
                        mouse.press (from);
                        mouse.dragTo (from.translated (screen.widthOf (1.0), 0));
                        phazite::deleteNotes (screen.context, { note });
                        phazite::test::deliverChanges();
                        phazite::test::dispatchPendingMessages();
                        mouse.dragTo (from.translated (screen.widthOf (3.0), -screen.rowHeight()));
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();

        CHECK_FALSE (screen.has (note));
        CHECK (screen.getAll().empty());
        undo.walkBackAndForth();
    }

    SUBCASE ("An automation point, and a tempo change: the one after it isn't dragged in its place")
    {
        LanesScreen screen;
        phazite::addAutomationPoint (screen.context, screen.volume, { beatAt (4.0), 0.5f });
        phazite::test::deliverChanges();
        phazite::addAutomationPoint (screen.context, screen.volume, { beatAt (12.0), 0.25f });
        phazite::test::deliverChanges();
        phazite::addTempoChange (screen.context, beatAt (16.0), 100.0);
        screen.getSong().getUndoManager().clearUndoHistory();
        phazite::test::dispatchPendingMessages();
        UndoCheck undo (screen.getSong());

        Mouse laneMouse (screen.playlist);
        undo.check ("a point's move whose point goes",
                    [&]
                    {
                        laneMouse.press (screen.inLane (4.0, 0.5f));
                        laneMouse.dragTo (screen.inLane (5.0, 0.5f));
                        phazite::deleteAutomationPoints (screen.context, screen.volume, { 0 });
                        phazite::test::deliverChanges();
                        phazite::test::dispatchPendingMessages();
                        laneMouse.dragTo (screen.inLane (7.0, 0.9f));
                        laneMouse.release();
                        laneMouse.pause();
                    });
        phazite::test::deliverChanges();

        REQUIRE (screen.getPoints().size() == 1);
        CHECK (screen.getPoints()[0].beat.inBeats() == doctest::Approx (12.0));
        CHECK (screen.getPoints()[0].value == doctest::Approx (0.25f));

        Mouse tempoMouse (*screen.tempoLane);
        undo.check ("a tempo change's move whose tempo change goes",
                    [&]
                    {
                        const auto from = screen.tempoPoint (1);
                        tempoMouse.press (from);
                        tempoMouse.dragTo (from.translated (screen.widthOf (1.0), 0));
                        phazite::deleteTempoChange (screen.context, 1);
                        phazite::test::dispatchPendingMessages();
                        tempoMouse.dragTo (from.translated (screen.widthOf (2.0), -8));
                        tempoMouse.release();
                        tempoMouse.pause();
                    });
        phazite::test::deliverChanges();

        REQUIRE (screen.getTempos().size() == 2);
        CHECK (screen.getTempos()[1].beat.inBeats() == doctest::Approx (16.0));
        CHECK (screen.getTempos()[1].bpm == doctest::Approx (100.0));
        undo.walkBackAndForth();
    }
}

//==============================================================================
TEST_CASE ("Another project opened in the middle of a drag ends it in the song it began in, and the new song, whose items have the "
           "same IDs, is left alone")
{
    // The new project is a copy of the song open, every ID the same: what a drag holds by its IDs would find its items there.
    const auto openCopy = [] (phazite::ProjectContext& context, phazite::Song& song)
    {
        auto copy = phazite::makeSong (song.getState().createCopy());
        const auto before = writeSong (*copy, SongPart::undoable);
        setSong (context, copy.get());
        phazite::test::dispatchPendingMessages();
        return std::pair (copy, before);
    };

    const auto checkLeftAlone = [] (const std::pair<phazite::Song::Ptr, juce::String>& opened)
    {
        CHECK (writeSong (*opened.first, SongPart::undoable) == opened.second);
        CHECK_FALSE (opened.first->getUndoManager().canUndo());
    };

    SUBCASE ("A clip, in the playlist")
    {
        std::pair<phazite::Song::Ptr, juce::String> opened;
        PlaylistScreen screen;
        const auto clip = screen.addMidiClip (0, 0.0, 2.0);
        phazite::test::dispatchPendingMessages();
        screen.getSong().getUndoManager().clearUndoHistory();
        const phazite::Song::Ptr song (&screen.getSong());
        Mouse mouse (*screen.lanes);
        UndoCheck undo (*song);

        undo.check ("a move, another project opened in it",
                    [&]
                    {
                        mouse.press (screen.lower (1.0, 0));
                        mouse.dragTo (screen.lower (3.0, 0));
                        opened = openCopy (screen.context, *song);
                        mouse.dragTo (screen.lower (7.0, 1));
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();

        checkLeftAlone (opened);
        CHECK (phazite::getClipRange (*song, clip).getStart().inBeats() == doctest::Approx (2.0));
        undo.walkBackAndForth();
        setSong (screen.context, nullptr);
    }

    SUBCASE ("A note, and a velocity, in the piano roll")
    {
        std::pair<phazite::Song::Ptr, juce::String> opened;
        PianoRollScreen screen;
        const auto note = screen.addNote (1.0);
        screen.getSong().getUndoManager().clearUndoHistory();
        const phazite::Song::Ptr song (&screen.getSong());
        UndoCheck undo (*song);
        Mouse mouse (*screen.notes);

        undo.check ("a move, another project opened in it",
                    [&]
                    {
                        const auto from = screen.middleOf (note);
                        mouse.press (from);
                        mouse.dragTo (from.translated (screen.widthOf (1.0), 0));
                        opened = openCopy (screen.context, *song);
                        mouse.dragTo (from.translated (screen.widthOf (3.0), -screen.rowHeight()));
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();

        checkLeftAlone (opened);
        CHECK (phazite::getClipNotes (*song, screen.clip).front().start.inBeats() == doctest::Approx (2.0));

        // The velocity lane's, back in the first song.
        setSong (screen.context, song.get());
        screen.selection().selectOnly (screen.clip);
        phazite::test::dispatchPendingMessages();
        Mouse laneMouse (*screen.velocity);
        const auto x = screen.xOf (2.0);
        undo.check ("a velocity drawn, another project opened in it",
                    [&]
                    {
                        laneMouse.press ({ x, 10 });
                        phazite::test::deliverChanges();
                        laneMouse.dragTo ({ x, 20 });
                        phazite::test::deliverChanges();
                        opened = openCopy (screen.context, *song);
                        laneMouse.dragTo ({ x, screen.velocity->getHeight() - 3 });
                        phazite::test::deliverChanges();
                        laneMouse.release();
                        laneMouse.pause();
                    });
        phazite::test::deliverChanges();

        checkLeftAlone (opened);
        undo.walkBackAndForth();
        setSong (screen.context, nullptr);
    }

    SUBCASE ("An automation point and a tempo change, in their lanes")
    {
        std::pair<phazite::Song::Ptr, juce::String> opened;
        LanesScreen screen;
        phazite::addAutomationPoint (screen.context, screen.volume, { beatAt (4.0), 0.5f });
        phazite::test::deliverChanges();
        screen.getSong().getUndoManager().clearUndoHistory();
        phazite::test::dispatchPendingMessages();
        const phazite::Song::Ptr song (&screen.getSong());
        UndoCheck undo (*song);

        Mouse laneMouse (screen.playlist);
        const auto later = screen.inLane (7.0, 0.9f); // found while the lane is there: another project's lanes are others

        undo.check ("a point's move, another project opened in it",
                    [&]
                    {
                        laneMouse.press (screen.inLane (4.0, 0.5f));
                        laneMouse.dragTo (screen.inLane (5.0, 0.5f));
                        opened = openCopy (screen.context, *song);
                        laneMouse.dragTo (later);
                        laneMouse.release();
                        laneMouse.pause();
                    });
        phazite::test::deliverChanges();

        checkLeftAlone (opened);

        setSong (screen.context, song.get());
        phazite::test::dispatchPendingMessages();
        screen.tempoLane = dynamic_cast<phazite::ui::TempoLane*> (phazite::test::findComponentWithTitle (screen.playlist, "Tempo"));
        REQUIRE (screen.tempoLane != nullptr);
        Mouse tempoMouse (*screen.tempoLane);
        undo.check ("a tempo change's move, another project opened in it",
                    [&]
                    {
                        const auto from = screen.tempoPoint (1);
                        tempoMouse.press (from);
                        tempoMouse.dragTo (from.translated (screen.widthOf (1.0), 0));
                        opened = openCopy (screen.context, *song);
                        tempoMouse.dragTo (from.translated (screen.widthOf (3.0), -8));
                        tempoMouse.release();
                        tempoMouse.pause();
                    });
        phazite::test::deliverChanges();

        checkLeftAlone (opened);
        undo.walkBackAndForth();
        setSong (screen.context, nullptr);
    }

    SUBCASE ("An audio clip's edge, in the audio clip editor")
    {
        std::pair<phazite::Song::Ptr, juce::String> opened;
        AudioScreen screen;
        UndoCheck undo (*screen.song);
        Mouse mouse (screen.waveform());
        const auto middleY = screen.waveform().getHeight() / 2;

        undo.check ("a trim, another project opened in it",
                    [&]
                    {
                        mouse.press (screen.trimStartGrip());
                        mouse.dragTo ({ screen.xOf (5.1), middleY });
                        phazite::test::deliverChanges();
                        opened = openCopy (screen.context, *screen.song);
                        mouse.dragTo ({ screen.xOf (7.1), middleY });
                        phazite::test::deliverChanges();
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();

        checkLeftAlone (opened);
        CHECK (phazite::getClipRange (*screen.song, screen.clip).getStart().inBeats() == doctest::Approx (5.0));
        undo.walkBackAndForth();
        setSong (screen.context, nullptr);
    }
}

//==============================================================================
TEST_CASE ("Escape in the middle of a drag takes back all it did in every editor, leaving nothing to undo or redo")
{
    const auto checkNothingLeft = [] (phazite::Song& song)
    {
        CHECK_FALSE (song.getUndoManager().canUndo());
        CHECK_FALSE (song.getUndoManager().canRedo());
    };

    SUBCASE ("Clips in the playlist: moved, copied, trimmed, and placed by the Draw tool")
    {
        PlaylistScreen screen;
        const auto clip = screen.addMidiClip (0, 0.0, 2.0);
        phazite::test::dispatchPendingMessages();
        screen.getSong().getUndoManager().clearUndoHistory();
        screen.selection().clear();
        Mouse mouse (*screen.lanes);
        UndoCheck undo (screen.getSong());

        for (const auto mods : { juce::ModifierKeys(), ctrl })
        {
            undo.check ("a move or a copy, then Escape",
                        [&]
                        {
                            mouse.press (screen.lower (1.0, 0), mods);
                            mouse.dragTo (screen.lower (3.0, 0));
                            mouse.dragTo (screen.lower (5.0, 1));
                            screen.window.pressKey (escape);
                            mouse.dragTo (screen.lower (7.0, 1)); // the mouse does nothing more until it goes up
                            mouse.release();
                            mouse.pause();
                        });
            phazite::test::deliverChanges();

            CHECK (screen.startsOn (0) == std::vector { 0.0 });
            phazite::test::deliverChanges();
            CHECK (screen.startsOn (1).empty());
            CHECK (screen.selection().getClips().empty()); // as it was when the mouse went down
        }

        undo.check ("a trim, then Escape",
                    [&]
                    {
                        const auto edge = screen.boundsOf (clip);
                        const juce::Point<int> grip (edge.getRight() - 2, edge.getBottom() - 4);
                        mouse.press (grip);
                        mouse.dragTo ({ screen.xOf (5.0), grip.y });
                        phazite::test::deliverChanges();
                        screen.window.pressKey (escape);
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();

        CHECK (screen.get (clip).range.getEnd().inBeats() == doctest::Approx (2.0));

        screen.playlist.setTool (Tool::draw);
        undo.check ("a clip placed, then Escape",
                    [&]
                    {
                        mouse.press (screen.lower (9.0, 0));
                        mouse.dragTo (screen.lower (11.0, 0));
                        screen.window.pressKey (escape);
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();

        CHECK (screen.startsOn (0) == std::vector { 0.0 });
        phazite::test::deliverChanges();
        CHECK (screen.playlist.getTool() == Tool::draw); // Escape took the drag back, and chose no tool
        CHECK (undo.getNumSteps() == 0);
        checkNothingLeft (screen.getSong());
    }

    SUBCASE ("A note in the piano roll, by the window's keys")
    {
        PianoRollScreen screen;
        const auto note = screen.addNote (1.0);
        screen.getSong().getUndoManager().clearUndoHistory();
        Mouse mouse (*screen.notes);
        UndoCheck undo (screen.getSong());

        undo.check ("a copy, then Escape",
                    [&]
                    {
                        const auto from = screen.middleOf (note);
                        mouse.press (from, ctrl);
                        mouse.dragTo (from.translated (screen.widthOf (2.0), 0));
                        screen.window.pressKey (escape);
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();

        CHECK (screen.getAll().size() == 1);
        CHECK (undo.getNumSteps() == 0);
        checkNothingLeft (screen.getSong());
    }

    SUBCASE ("Automation points and tempo changes in their lanes")
    {
        LanesScreen screen;
        phazite::addAutomationPoint (screen.context, screen.volume, { beatAt (4.0), 0.5f });
        phazite::test::deliverChanges();
        screen.getSong().getUndoManager().clearUndoHistory();
        phazite::test::dispatchPendingMessages();
        UndoCheck undo (screen.getSong());

        Mouse laneMouse (screen.playlist);
        undo.check ("a point's move, then Escape",
                    [&]
                    {
                        laneMouse.press (screen.inLane (4.0, 0.5f));
                        laneMouse.dragTo (screen.inLane (6.0, 0.8f));
                        screen.window.pressKey (escape);
                        laneMouse.dragTo (screen.inLane (8.0, 0.8f));
                        laneMouse.release();
                        laneMouse.pause();
                    });
        phazite::test::deliverChanges();

        REQUIRE (screen.getPoints().size() == 1);
        CHECK (screen.getPoints()[0].beat.inBeats() == doctest::Approx (4.0));

        // A point pulled from the line, which goes too.
        undo.check ("a point pulled from the line, then Escape",
                    [&]
                    {
                        laneMouse.press (screen.inLane (10.0, 0.5f));
                        laneMouse.dragTo (screen.inLane (10.0, 0.9f));
                        screen.window.pressKey (escape);
                        laneMouse.release();
                        laneMouse.pause();
                    });
        phazite::test::deliverChanges();

        CHECK (screen.getPoints().size() == 1);

        Mouse tempoMouse (*screen.tempoLane);
        undo.check ("a tempo change's move, then Escape",
                    [&]
                    {
                        const auto from = screen.tempoPoint (1);
                        tempoMouse.press (from);
                        tempoMouse.dragTo (from.translated (screen.widthOf (2.0), -10));
                        screen.window.pressKey (escape);
                        tempoMouse.release();
                        tempoMouse.pause();
                    });
        phazite::test::deliverChanges();

        REQUIRE (screen.getTempos().size() == 2);
        CHECK (screen.getTempos()[1].beat.inBeats() == doctest::Approx (8.0));
        CHECK (screen.getTempos()[1].bpm == doctest::Approx (140.0));
        CHECK (undo.getNumSteps() == 0);
        checkNothingLeft (screen.getSong());
    }

    SUBCASE ("An audio clip's edge in the audio clip editor")
    {
        AudioScreen screen;
        UndoCheck undo (*screen.song);
        Mouse mouse (screen.waveform());

        undo.check ("a trim, then Escape",
                    [&]
                    {
                        mouse.press (screen.trimStartGrip());
                        mouse.dragTo ({ screen.xOf (6.1), screen.waveform().getHeight() / 2 });
                        phazite::test::deliverChanges();
                        screen.window.pressKey (escape);
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();

        CHECK (screen.get().clip.range.getStart().inBeats() == doctest::Approx (4.0));
        CHECK (undo.getNumSteps() == 0);
        checkNothingLeft (*screen.song);
    }
}

//==============================================================================
TEST_CASE ("The copies of a Ctrl+drag go as soon as the drag comes back to where it began, and come again as it leaves")
{
    SUBCASE ("Clips")
    {
        PlaylistScreen screen;
        const auto clip = screen.addMidiClip (0, 0.0, 2.0);
        phazite::test::dispatchPendingMessages();
        screen.getSong().getUndoManager().clearUndoHistory();
        Mouse mouse (*screen.lanes);
        UndoCheck undo (screen.getSong());

        undo.check ("a copy dragged out, back and out again",
                    [&]
                    {
                        mouse.press (screen.lower (1.0, 0), ctrl);
                        mouse.dragTo (screen.lower (3.0, 0));
                        CHECK (screen.startsOn (0) == std::vector { 0.0, 2.0 });
                        phazite::test::deliverChanges();

                        // Known bug 21 until 2026-11-14: the copy lay on its original until the mouse went up.
                        mouse.dragTo (screen.lower (1.1, 0));
                        CHECK (screen.startsOn (0) == std::vector { 0.0 });
                        phazite::test::deliverChanges();
                        CHECK (screen.selection().getClips() == std::vector { clip });
                        phazite::test::deliverChanges();

                        mouse.dragTo (screen.lower (5.0, 0));
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();

        CHECK (screen.startsOn (0) == std::vector { 0.0, 4.0 });
        phazite::test::deliverChanges();
        CHECK (screen.describeUndo() == "Undo Copy Clip");

        // Back where it began as the mouse goes up: nothing, and no step.
        undo.check ("a copy dragged out and back",
                    [&]
                    {
                        mouse.press (screen.lower (1.0, 0), ctrl);
                        mouse.dragTo (screen.lower (3.0, 0));
                        mouse.dragTo (screen.lower (1.0, 0));
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();

        CHECK (screen.startsOn (0) == std::vector { 0.0, 4.0 });
        phazite::test::deliverChanges();
        CHECK (undo.getNumSteps() == 1);
        undo.walkBackAndForth();
    }

    SUBCASE ("Notes")
    {
        PianoRollScreen screen;
        const auto note = screen.addNote (1.0);
        screen.getSong().getUndoManager().clearUndoHistory();
        Mouse mouse (*screen.notes);
        UndoCheck undo (screen.getSong());
        const auto from = screen.middleOf (note);

        undo.check ("a copy dragged out, back and out again",
                    [&]
                    {
                        mouse.press (from, ctrl);
                        mouse.dragTo (from.translated (screen.widthOf (2.0), 0));
                        CHECK (screen.getAll().size() == 2);

                        // Known bug 21 until 2026-11-14.
                        mouse.dragTo (from.translated (2, 0));
                        CHECK (screen.getAll().size() == 1);
                        CHECK (screen.getSelected() == std::vector { note });
                        phazite::test::deliverChanges();

                        // A tool's key in the middle waits for the mouse to go up, the copy made again.
                        screen.window.pressKey (getKey ("T"));
                        screen.window.releaseKeys();
                        mouse.dragTo (from.translated (screen.widthOf (3.0), 0));
                        CHECK (screen.getAll().size() == 2);
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();

        REQUIRE (screen.getAll().size() == 2);
        CHECK (screen.getAll()[1].start.inBeats() == doctest::Approx (4.0));
        CHECK (screen.describeUndo() == "Undo Copy Notes");
        CHECK (screen.pianoRoll.getTool() == Tool::mute);
        undo.walkBackAndForth();
    }
}

//==============================================================================
TEST_CASE ("Automation points and tempo changes off the grid keep their offset from it as they are dragged (known bug 19)")
{
    LanesScreen screen;
    phazite::addAutomationPoint (screen.context, screen.volume, { beatAt (4.3), 0.5f });
    phazite::test::deliverChanges();
    phazite::moveTempoChange (screen.context, 1, beatAt (8.3), 140.0);
    screen.getSong().getUndoManager().clearUndoHistory();
    phazite::test::dispatchPendingMessages();
    UndoCheck undo (screen.getSong());

    Mouse laneMouse (screen.playlist);
    undo.check ("a point dragged up",
                [&]
                {
                    const auto from = screen.inLane (4.3, 0.5f);
                    laneMouse.press (from);
                    laneMouse.dragTo (from.translated (0, -8));
                    laneMouse.dragTo (from.translated (1, -20));
                    laneMouse.release();
                    laneMouse.pause();
                });
    phazite::test::deliverChanges();

    REQUIRE (screen.getPoints().size() == 1);
    CHECK (screen.getPoints()[0].beat.inBeats() == doctest::Approx (4.3));
    CHECK (screen.getPoints()[0].value > 0.55f);

    undo.check ("a point dragged two beats and a bit along",
                [&]
                {
                    const auto from = screen.inLane (4.3, screen.getPoints()[0].value);
                    laneMouse.press (from);
                    laneMouse.dragTo (from.translated (screen.widthOf (1.0), 0));
                    laneMouse.dragTo (from.translated (screen.widthOf (2.2), 0));
                    laneMouse.release();
                    laneMouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK (screen.getPoints()[0].beat.inBeats() == doctest::Approx (6.3));

    Mouse tempoMouse (*screen.tempoLane);
    undo.check ("a tempo change dragged down",
                [&]
                {
                    const auto from = screen.tempoPoint (1);
                    tempoMouse.press (from);
                    tempoMouse.dragTo (from.translated (0, 6));
                    tempoMouse.dragTo (from.translated (1, 10));
                    tempoMouse.release();
                    tempoMouse.pause();
                });
    phazite::test::deliverChanges();

    REQUIRE (screen.getTempos().size() == 2);
    CHECK (screen.getTempos()[1].beat.inBeats() == doctest::Approx (8.3));
    CHECK (screen.getTempos()[1].bpm < 140.0);

    undo.check ("a tempo change dragged two beats and a bit along",
                [&]
                {
                    const auto from = screen.tempoPoint (1);
                    tempoMouse.press (from);
                    tempoMouse.dragTo (from.translated (screen.widthOf (1.0), 0));
                    tempoMouse.dragTo (from.translated (screen.widthOf (2.2), 0));
                    tempoMouse.release();
                    tempoMouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK (screen.getTempos()[1].beat.inBeats() == doctest::Approx (10.3));
    undo.walkBackAndForth();
}

//==============================================================================
TEST_CASE ("A trackpad's parts of a notch add up over each place the wheel acts in the editors, as whole notches would")
{
    SUBCASE ("The velocity lane: whole notches' steps, an undo step for each, not one for each part")
    {
        PianoRollScreen screen;
        const auto note = screen.addNote (1.0, 0, 1.0, 60);
        screen.getSong().getUndoManager().clearUndoHistory();
        Mouse mouse (*screen.velocity);
        UndoCheck undo (screen.getSong());

        // Five notches in 80 parts: five steps of 4. Taken by its direction, each part raised the velocity a step and made an
        // undo step of its own: to 127, in 80 steps (known bug 55 until 2026-11-14).
        undo.checkSeveral ("a trackpad's scroll",
                           [&]
                           {
                               for (int i = 0; i < 80; ++i)
                                   mouse.wheel ({ screen.xOf (1.0), 20 }, partOfNotch (16));
                           });
        phazite::test::deliverChanges();

        CHECK (screen.get (note).velocity == 80);
        CHECK (undo.getNumSteps() == 5);
        undo.walkBackAndForth();
    }

    SUBCASE ("The piano roll's rows: their height with Alt, scrolled without")
    {
        PianoRollScreen screen;
        Mouse mouse (*screen.notes);
        const auto rowsBefore = screen.rowHeight();

        for (int i = 0; i < 32; ++i)
            mouse.wheel (screen.at (2.0), partOfNotch (32), juce::ModifierKeys::altModifier);

        CHECK (screen.rowHeight() == rowsBefore + 1); // a pixel a notch

        const auto scrollBefore = screen.pianoRoll.getScrollY();

        for (int i = 0; i < 32; ++i)
            mouse.wheel (screen.at (2.0), partOfNotch (32));

        CHECK (screen.pianoRoll.getScrollY() == scrollBefore - 48); // a notch's 48 pixels, up

        const auto zoomBefore = screen.pianoRoll.getView().pixelsPerBeat;

        for (int i = 0; i < 16; ++i)
            mouse.wheel (screen.at (2.0), partOfNotch (16), juce::ModifierKeys::commandModifier);

        CHECK (screen.pianoRoll.getView().pixelsPerBeat == doctest::Approx (zoomBefore * 1.25));
        CHECK (screen.getAll().empty());
    }

    SUBCASE ("The playlist: the tracks' height with Alt, scrolled up and down, and sideways with Shift")
    {
        PlaylistScreen screen;
        phazite::ensureTracks (screen.getSong(), 24);
        screen.getSong().getUndoManager().clearUndoHistory();
        phazite::test::dispatchPendingMessages();
        Mouse mouse (*screen.lanes);
        UndoCheck undo (screen.getSong());

        // A quarter of a pixel at a time: rounded each on its own, nothing.
        undo.check ("Alt and the wheel",
                    [&]
                    {
                        for (int i = 0; i < 32; ++i)
                            mouse.wheel (screen.at (2.0, 0), partOfNotch (32), juce::ModifierKeys::altModifier);
                    });
        phazite::test::deliverChanges();

        CHECK (screen.heightOf (0) == phazite::ui::TrackHeight::standard + phazite::ui::TrackHeight::step / 2);
        CHECK (undo.getNumSteps() == 0); // how tall tracks show is no change to the song

        auto* rows = [&screen]() -> juce::ScrollBar*
        {
            for (auto* bar : phazite::test::findAll<juce::ScrollBar> (screen.playlist))
                if (bar->isVertical())
                    return bar;

            return nullptr;
        }();

        REQUIRE (rows != nullptr);

        // Down a notch, in parts of a pixel and a half each: rounded each on its own, two.
        for (int i = 0; i < 32; ++i)
            mouse.wheel (screen.at (2.0, 0), -partOfNotch (32));

        CHECK (rows->getCurrentRangeStart() == doctest::Approx (48.0));

        const auto startBefore = screen.playlist.getView().start.inBeats();

        for (int i = 0; i < 32; ++i)
            mouse.wheel (screen.at (2.0, 0), -partOfNotch (32), juce::ModifierKeys::shiftModifier);

        CHECK ((screen.playlist.getView().start.inBeats() - startBefore) * screen.playlist.getView().pixelsPerBeat
               == doctest::Approx (48.0));
    }
}

//==============================================================================
namespace
{
// The files under a folder, each with its size and when it was written, a line each.
juce::String describeFiles (const juce::File& folder)
{
    juce::StringArray lines;

    for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
        lines.add (entry.getFile().getRelativePathFrom (folder) + " " + juce::String (entry.getFileSize()) + " "
                   + juce::String (entry.getModificationTime().toMilliseconds()));

    lines.sort (false);
    return lines.joinIntoString ("\n");
}

// What a song's undo history holds: its steps' names either way, and the changes in the step in progress (a drag's).
juce::String describeHistory (phazite::Song& song)
{
    const auto& history = song.getUndoManager();
    return history.getUndoDescriptions().joinIntoString (", ") + " | " + history.getRedoDescriptions().joinIntoString (", ") + " | "
           + juce::String (history.getNumActionsInCurrentTransaction());
}

// The first of Phazite's own devices, on a track.
phazite::DeviceID addOwnDevice (phazite::ProjectContext& context, phazite::TrackID track)
{
    for (const auto& type : phazite::getDeviceTypes (context))
        if (type.isBuiltIn)
            return phazite::addDeviceTo (*phazite::findSong (context), track, type, nullptr);

    return {};
}
} // namespace

TEST_CASE ("While the mouse drags in the playlist or the piano roll, no command the app registers changes the song, its history or "
           "its files from its key")
{
    // The app's window, with every command it registers, as AppShellTests.cpp's catalogue lists them, and its keys on it, as
    // the main window's are (ui/input/WindowKeys.h). Each command in turn gets a key of its own, which is pressed in the
    // middle of a drag: whatever it is, the song, its undo history and the project's files are as they were. A command that
    // could change them waits for the mouse to go up (ui/input/KeyDispatcher.h): Capture and Consolidate, which begin undo
    // steps of their own, joined the drag's step, which Escape then took back with them (2026-11-14).
    phazite::test::AppScreen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    screen.window.addKeyListener (&screen.shortcutKeys);
    auto& context = screen.session.getContext();
    auto& song = *phazite::findSong (context);
    const auto tracks = phazite::ensureTracks (song, 2);
    const auto clip = phazite::insertMidiClip (song, tracks[0], { beatAt (0.0), beatAt (8.0) }, nullptr);
    REQUIRE (clip.isValid());
    song.getUndoManager().clearUndoHistory();
    phazite::test::dispatchPendingMessages();
    const auto project = context.getProjectNumber();

    const auto projectFiles = [&screen]
    {
        return describeFiles (screen.temp.folder.getChildFile ("Projects")) + "\n"
               + describeFiles (screen.temp.folder.getChildFile ("Unsaved"));
    };

    const auto pressEveryCommandsKey = [&]
    {
        const auto key = juce::KeyPress (juce::KeyPress::F20Key);
        auto& mappings = screen.commands.getKeyMappings();
        REQUIRE (mappings.findCommandForKeyPress (key) == 0);

        for (int i = 0; i < screen.registry.getNumCommands(); ++i)
        {
            const auto id = screen.registry.getCommand (i)->id;
            CAPTURE (id.toStdString());
            const auto text = writeSong (song, SongPart::whole);
            const auto history = describeHistory (song);
            const auto files = projectFiles();
            mappings.addKeyPress (screen.commands.getCommandID (id), key);
            screen.window.keyGoesDown();
            screen.window.pressKey (key);
            screen.window.releaseKeys();
            mappings.removeKeyPress (key);

            REQUIRE (phazite::findSong (context) == &song);
            REQUIRE (context.getProjectNumber() == project);
            CHECK (phazite::test::describeSongDifferences (text, writeSong (song, SongPart::whole)).isEmpty());
            CHECK (describeHistory (song) == history);
            CHECK (projectFiles() == files);
        }
    };

    UndoCheck undo (song);

    // A copy of a clip dragged in the playlist.
    {
        auto& playlist = screen.showView<phazite::ui::Playlist> ("view.playlist");
        auto* lanes = phazite::test::findComponentWithTitle (playlist, "Tracks");
        REQUIRE (lanes != nullptr);
        juce::Rectangle<int> bounds;

        for (auto* component : phazite::test::findAll<phazite::ui::ClipComponent> (playlist))
            if (component->getClip() == clip)
                bounds = lanes->getLocalArea (component, component->getLocalBounds());

        REQUIRE_FALSE (bounds.isEmpty());
        Mouse mouse (*lanes);
        const auto grip = bounds.getCentre().translated (0, bounds.getHeight() / 4); // the lower half moves it

        undo.check ("a copy dragged with every command's key pressed in it",
                    [&]
                    {
                        mouse.press (grip, ctrl);
                        mouse.dragTo (grip.translated (10, 0));
                        mouse.dragTo (grip.translated (juce::roundToInt (4.0 * playlist.getView().pixelsPerBeat), 0));
                        pressEveryCommandsKey();
                        mouse.dragTo (grip.translated (juce::roundToInt (4.0 * playlist.getView().pixelsPerBeat) + 1, 0));
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();

        if (phazite::isPlaying (context))
            REQUIRE (screen.registry.invoke ("transport.playStop"));

        CHECK (phazite::getClips (context, tracks[0]).size() == 2);
    }

    // A note dragged in the piano roll.
    {
        phazite::getSelection (context).selectOnly (clip);
        phazite::test::dispatchPendingMessages();
        auto& roll = screen.showView<phazite::ui::PianoRoll> ("view.pianoRoll");
        auto* notes = phazite::test::findComponentWithTitle (roll, "Notes");
        REQUIRE (notes != nullptr);
        Mouse mouse (*notes);
        const juce::Point<int> at (juce::roundToInt (roll.getView().beatToX (beatAt (1.1))), notes->getHeight() / 2);
        mouse.click (at); // a note
        phazite::test::deliverChanges();
        REQUIRE (phazite::getNotes (context, { clip }).size() == 1);

        undo.check ("a note dragged with every command's key pressed in it",
                    [&]
                    {
                        mouse.press (at);
                        mouse.dragTo (at.translated (10, 0));
                        mouse.dragTo (at.translated (juce::roundToInt (2.0 * roll.getView().pixelsPerBeat), 0));
                        pressEveryCommandsKey();
                        mouse.dragTo (at.translated (juce::roundToInt (2.0 * roll.getView().pixelsPerBeat) + 1, 0));
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();

        if (phazite::isPlaying (context))
            REQUIRE (screen.registry.invoke ("transport.playStop"));

        CHECK (phazite::getNotes (context, { clip }).size() == 1);
    }

    undo.walkBackAndForth();
    screen.window.removeKeyListener (&screen.shortcutKeys);
}

TEST_CASE ("Escape puts back exactly what was selected as the mouse went down, whatever it was: a device, notes, points or clips")
{
    SUBCASE ("In the playlist's lanes")
    {
        LanesScreen screen;
        const auto clip = screen.addMidiClip (1, 0.0, 2.0), other = screen.addMidiClip (1, 4.0, 2.0);
        const auto device = addOwnDevice (screen.context, screen.tracks[2]);
        REQUIRE (device.isValid());
        const auto note = phazite::addNote (screen.context, other, 60, beatAt (4.0), phazite::BeatDuration::fromBeats (1.0), 100);
        phazite::addAutomationPoint (screen.context, screen.volume, { beatAt (4.0), 0.5f });
        screen.getSong().getUndoManager().clearUndoHistory();
        phazite::test::dispatchPendingMessages();
        auto& selection = screen.selection();
        Mouse mouse (*screen.lanes);
        const auto onClip = screen.lower (1.0, 1).translated (0, phazite::AutomationLane::defaultHeight);

        const auto dragAndEscape = [&]
        {
            mouse.press (onClip);
            mouse.dragTo (onClip.translated (10, 0));
            mouse.dragTo (onClip.translated (screen.widthOf (2.0), 0));
            phazite::test::deliverChanges();
            screen.window.pressKey (escape);
            mouse.release();
            mouse.pause();
            phazite::test::deliverChanges();
            CHECK (screen.get (clip).range.getStart().inBeats() == doctest::Approx (0.0));
        };

        selection.selectOnly (device);
        dragAndEscape();
        CHECK (selection.getDevices() == std::vector { device });
        CHECK (selection.getClips().empty());

        selection.setNoteClips ({ other });
        selection.selectNotes ({ note });
        dragAndEscape();
        CHECK (selection.getNotes() == std::vector { note });

        selection.selectAutomationPoints (screen.volume, { 0 });
        dragAndEscape();
        CHECK (selection.getSelectedAutomationPoints().parameter == screen.volume);
        CHECK (selection.getSelectedAutomationPoints().indices == std::vector { 0 });

        // A point's drag in its lane, with a device selected.
        selection.selectOnly (device);
        Mouse laneMouse (screen.playlist);
        laneMouse.press (screen.inLane (4.0, 0.5f));
        laneMouse.dragTo (screen.inLane (6.0, 0.8f));
        screen.window.pressKey (escape);
        laneMouse.release();
        laneMouse.pause();
        phazite::test::deliverChanges();
        CHECK (selection.getDevices() == std::vector { device });
        CHECK (screen.getPoints()[0].beat.inBeats() == doctest::Approx (4.0));
        CHECK_FALSE (screen.getSong().getUndoManager().canUndo());
    }

    SUBCASE ("In the piano roll")
    {
        PianoRollScreen screen;
        const auto note = screen.addNote (1.0);
        const auto device = addOwnDevice (screen.context, screen.tracks[1]);
        REQUIRE (device.isValid());
        screen.getSong().getUndoManager().clearUndoHistory();
        phazite::test::dispatchPendingMessages();
        auto& selection = screen.selection();
        Mouse mouse (*screen.notes);

        const auto dragAndEscape = [&]
        {
            const auto from = screen.middleOf (note);
            mouse.press (from);
            mouse.dragTo (from.translated (screen.widthOf (2.0), 0));
            phazite::test::deliverChanges();
            screen.window.pressKey (escape);
            mouse.release();
            mouse.pause();
            phazite::test::deliverChanges();
            CHECK (screen.get (note).start.inBeats() == doctest::Approx (1.0));
        };

        // The clip, as the playlist selects it, and a device.
        selection.selectOnly (screen.clip);
        dragAndEscape();
        CHECK (selection.getClips() == std::vector { screen.clip });
        CHECK (selection.getNotes().empty());

        selection.selectOnly (device);
        dragAndEscape();
        CHECK (selection.getDevices() == std::vector { device });
        CHECK (selection.getNotes().empty());
        CHECK_FALSE (screen.getSong().getUndoManager().canUndo());
    }
}