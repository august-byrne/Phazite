// SPDX-License-Identifier: AGPL-3.0-or-later
// The piano roll, driven with the mouse and the selection as the user would.
#include "GoldenFiles.h"
#include "UiTestHelpers.h"
#include "model/arrangement/Clips.h"
#include "model/arrangement/Selection.h"
#include "model/arrangement/Tracks.h"
#include "model/commands/Undo.h"
#include "model/mixer/Mixer.h"
#include "model/notes/Notes.h"
#include "model/transport/Playback.h"
#include "ui/pianoroll/PianoRoll.h"

namespace
{
te::BeatPosition beats (double b)
{
    return te::BeatPosition::fromBeats (b);
}

// A piano roll in a window, with two tracks and the commands the app gives it. It follows the selection.
struct PianoRollScreen
{
    PianoRollScreen()
    {
        edit->ensureNumberOfAudioTracks (2);
        tracks = te::getAudioTracks (*edit);
        edit->getUndoManager().clearUndoHistory();
        selection.edit = edit.get();

        phazite::ui::addTimelineCommands (registry, [this] { return &pianoRoll; });
        pianoRoll.addCommands (registry);
        pianoRoll.setEdit (edit.get());
        phazite::test::dispatchPendingMessages();

        notes = phazite::test::findComponentWithTitle (pianoRoll, "Notes");
        REQUIRE (notes != nullptr);
    }

    ~PianoRollScreen()
    {
        pianoRoll.setEdit (nullptr);
        selection.deselectAll();
    }

    te::MidiClip& addClip (int track, double start, double length)
    {
        auto* clip = phazite::insertMidiClip (*tracks[track], { beats (start), beats (start + length) }, nullptr);
        REQUIRE (clip != nullptr);
        return *clip;
    }

    void select (te::Selectable& item)
    {
        selection.selectOnly (item);
        phazite::test::dispatchPendingMessages();
    }

    // Where a beat is in the note grid, halfway down.
    juce::Point<int> at (double beat) const
    {
        return { juce::roundToInt (pianoRoll.getView().beatToX (beats (beat))), notes->getHeight() / 2 };
    }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine()); // 120 BPM, 4/4
    juce::Array<te::AudioTrack*> tracks;
    te::SelectionManager selection { phazite::test::getEngine() };
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::PianoRoll pianoRoll { commands, selection };
    phazite::test::TestWindow window { pianoRoll, 1000, 600 };
    juce::Component* notes = nullptr;
};
} // namespace

TEST_CASE ("The piano roll keeps the notes being edited in view while the song plays, unless asked to follow it")
{
    PianoRollScreen screen;
    CHECK_FALSE (screen.pianoRoll.isFollowingPlayback());

    // View > Follow Playback turns it on for the editor used last, as the playlist has it from the start.
    REQUIRE (screen.registry.invoke ("view.followPlayback"));
    CHECK (screen.pianoRoll.isFollowingPlayback());
}

TEST_CASE ("While a clip linked to the one shown plays, the piano roll's playhead shows where its notes are playing")
{
    PianoRollScreen screen;
    auto& clip = screen.addClip (0, 0.0, 4.0);
    const auto aliases = phazite::copyClips ({ &clip }, true);
    REQUIRE (aliases.size() == 1);
    aliases[0]->setPosition ({ { te::TimePosition::fromSeconds (4.0), te::TimePosition::fromSeconds (6.0) }, {} }); // beats 8 to 12
    screen.select (clip);

    auto* playhead = phazite::test::findComponentWithTitle (screen.pianoRoll, "Playhead");
    REQUIRE (playhead != nullptr);
    const auto playheadAt = [&screen, playhead] (double beat)
    {
        return std::abs (playhead->getBounds().getCentreX() - (screen.notes->getX() + screen.pianoRoll.getView().beatToX (beats (beat))))
               <= 1.0;
    };

    // At beat 9.5 the alias plays beat 1.5 of the notes.
    phazite::movePlayhead (*screen.edit, te::TimePosition::fromSeconds (4.75));
    CHECK (phazite::test::dispatchMessagesUntil ([&screen] { return screen.pianoRoll.isPlayheadShowingLinkedClip(); }));
    CHECK (playhead->isVisible());
    CHECK (playheadAt (1.5));

    // In the clip itself, it is the song's own position again.
    phazite::movePlayhead (*screen.edit, te::TimePosition::fromSeconds (0.5));
    CHECK (phazite::test::dispatchMessagesUntil ([&screen] { return ! screen.pianoRoll.isPlayheadShowingLinkedClip(); }));
    CHECK (playheadAt (1.0));
}

TEST_CASE ("The piano roll counts bars from the start of the clip it shows")
{
    PianoRollScreen screen;
    auto& clip = screen.addClip (0, 32.0, 8.0); // at bar 9 of the song
    screen.select (clip);

    REQUIRE (screen.pianoRoll.getClips().contains (&clip));
    CHECK (screen.pianoRoll.getTimeOrigin().inBeats() == doctest::Approx (32.0));
    CHECK (screen.pianoRoll.getView().start.inBeats() == doctest::Approx (32.0)); // it opens at bar 1

    // Moved in the song, the clip still starts at bar 1, and the view stays on its notes.
    phazite::moveClip (clip, beats (48.0), *screen.tracks[0]);
    phazite::test::dispatchPendingMessages();
    CHECK (screen.pianoRoll.getTimeOrigin().inBeats() == doctest::Approx (48.0));
    CHECK (screen.pianoRoll.getView().start.inBeats() == doctest::Approx (48.0));
}

TEST_CASE ("The piano roll's grid counts from the start of its clip")
{
    // A clip half a beat into the song: its grid lines are half a beat off the song's.
    PianoRollScreen screen;
    screen.select (screen.addClip (0, 0.5, 8.0));

    phazite::GridSettings grid;
    grid.adaptive = false;
    grid.fixedLevel = 9; // a beat
    screen.pianoRoll.setGrid (grid);

    CHECK (screen.pianoRoll.snap (beats (2.7), {}).inBeats() == doctest::Approx (2.5));
    CHECK (screen.pianoRoll.snapDown (beats (2.4), {}).inBeats() == doctest::Approx (1.5));
}

TEST_CASE ("Selecting a track shows its clip nearest the playhead, or lets the first note start one")
{
    PianoRollScreen screen;
    screen.addClip (0, 0.0, 4.0);
    auto& later = screen.addClip (0, 16.0, 4.0);

    // The playhead is in neither clip: the nearer one shows.
    screen.edit->getTransport().setPosition (screen.edit->tempoSequence.toTime (beats (13.0)));
    screen.select (*screen.tracks[0]);
    CHECK (screen.pianoRoll.getClips() == juce::Array<te::MidiClip*> { &later });

    // A track without clips shows from the bar the playhead is in, and a click there starts a clip.
    screen.select (*screen.tracks[1]);
    CHECK (screen.pianoRoll.getTrack() == screen.tracks[1]);
    CHECK (screen.pianoRoll.getClips().isEmpty());
    CHECK (screen.pianoRoll.getTimeOrigin().inBeats() == doctest::Approx (12.0)); // bar 4, which beat 13 is in

    phazite::test::Mouse mouse (*screen.notes);
    mouse.click (screen.at (14.5));

    const auto clips = screen.tracks[1]->getClips();
    REQUIRE (clips.size() == 1);
    auto* started = dynamic_cast<te::MidiClip*> (clips.getFirst());
    REQUIRE (started != nullptr);
    CHECK (started->getStartBeat().inBeats() == doctest::Approx (12.0));
    CHECK (started->getSequence().getNumNotes() == 1);
    CHECK (screen.pianoRoll.getClips() == juce::Array<te::MidiClip*> { started });

    // One undo step takes back the clip with its note.
    screen.edit->getUndoManager().undo();
    CHECK (screen.tracks[1]->getClips().isEmpty());
}

TEST_CASE ("The piano roll paints notes, with and without a key")
{
    PianoRollScreen screen;
    auto& clip = screen.addClip (0, 0.0, 4.0);
    phazite::addNote (clip, 60, beats (0.0), te::BeatDuration::fromBeats (1.0), 100);
    phazite::addNote (clip, 64, beats (1.0), te::BeatDuration::fromBeats (1.0), 100);
    screen.select (clip);

    CHECK_FALSE (screen.pianoRoll.getTrack() == nullptr);
    CHECK (screen.window.paintEverything().getWidth() == 1000);

    phazite::setTrackKey (*screen.tracks[0], phazite::Key { 2, te::Scale::dorian });
    phazite::test::dispatchPendingMessages();
    CHECK (screen.window.paintEverything().getWidth() == 1000);
}

TEST_CASE ("On a track without an instrument, the piano roll asks for one, since its notes make no sound")
{
    PianoRollScreen screen;
    auto& clip = screen.addClip (0, 0.0, 4.0);
    screen.select (clip);

    auto* button = dynamic_cast<juce::Button*> (phazite::test::findComponentWithTitle (screen.pianoRoll, "Instrument"));
    REQUIRE (button != nullptr);
    CHECK (button->getButtonText() == juce::String ("Choose instrument"));

    auto synth = screen.edit->getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {});
    REQUIRE (phazite::setInstrument (*screen.tracks[0], synth) != nullptr);
    phazite::test::dispatchPendingMessages();
    CHECK (button->getButtonText() == synth->getName());
}

TEST_CASE ("A note drawn past its clip, where the next clip on the track starts, doesn't stretch the clip over it")
{
    PianoRollScreen screen;
    auto& first = screen.addClip (0, 0.0, 4.0);
    auto& next = screen.addClip (0, 4.0, 4.0);
    screen.select (first);

    phazite::test::Mouse mouse (*screen.notes);
    mouse.click (screen.at (4.5)); // after the clip shown: the note goes into it

    REQUIRE (first.getSequence().getNumNotes() == 1);
    CHECK (phazite::getNoteStart (first, *first.getSequence().getNotes().getFirst()).inBeats() > 3.999); // snapped to 4 or 4.5
    CHECK (first.getEndBeat().inBeats() == doctest::Approx (4.0));
    CHECK (next.getStartBeat().inBeats() == doctest::Approx (4.0));
    CHECK (next.getSequence().getNumNotes() == 0);

    screen.edit->getUndoManager().undo(); // one step
    CHECK (first.getSequence().getNumNotes() == 0);
}

TEST_CASE ("A note added and dragged is one undo step, however long the drag takes")
{
    PianoRollScreen screen;
    auto& clip = screen.addClip (0, 0.0, 8.0);
    screen.select (clip);

    phazite::test::Mouse mouse (*screen.notes);
    mouse.press (screen.at (1.0));
    REQUIRE (clip.getSequence().getNumNotes() == 1);
    mouse.dragTo (screen.at (2.0));
    phazite::test::settle (500); // Tracktion starts a new undo step 350 ms after a change, though not while the mouse is down
    mouse.dragTo (screen.at (3.0));
    mouse.release();

    CHECK (phazite::getNoteStart (clip, *clip.getSequence().getNotes().getFirst()).inBeats() > 2.5);
    CHECK (phazite::describeUndo (screen.edit.get()) == juce::String ("Undo Add Note"));
    phazite::undo (*screen.edit);
    CHECK (clip.getSequence().getNumNotes() == 0); // the note and all its moves, in one step
}
