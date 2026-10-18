// SPDX-License-Identifier: AGPL-3.0-or-later
// The audio clip editor (src/ui/audioclip/AudioClipEditor.h), driven with the mouse as the user would: it shows the
// selected audio clip in the song's bars; its edges trim the clip, its handles fade it and a drag moves the audio inside
// it; its fields and switches change the clip's gain, pitch, direction and tempo; and every change is one undo step (the
// roadmap's step 2.6: a vocal clip trimmed, faded, turned up, pitched and stretched to the tempo). And the editor dock's
// first tab, which shows the piano roll or the audio clip editor as the selection asks, named for what it shows.
#include "UiTestHelpers.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Selection.h"
#include "model/commands/internal/Undo.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/audioclip/AudioClipEditor.h"
#include "ui/pianoroll/PianoRoll.h"
#include "ui/shell/Panels.h"

namespace
{
using phazite::BeatPosition;

// An audio clip editor in a window, in a project at 120 BPM with a MIDI clip on track A from beat 4 to 8, and a "vocal"
// on track B from beat 4 to 12: a sine 4 seconds long.
struct EditorScreen
{
    EditorScreen()
    {
        edit->ensureNumberOfAudioTracks (2);
        a = te::getAudioTracks (*edit)[0];
        b = te::getAudioTracks (*edit)[1];
        setEdit (context, edit.get());

        auto* notes = phazite::insertMidiClip (*a, { te::BeatPosition::fromBeats (4.0), te::BeatPosition::fromBeats (8.0) }, nullptr);
        REQUIRE (notes != nullptr);
        midi = phazite::getClipID (*notes);

        const auto file = phazite::test::writeSound (temp.folder.getChildFile ("Vocal.wav"), 4.0);
        const auto imported =
            phazite::importFiles (context, { file.getFullPathName() }, phazite::getTrackID (*b), BeatPosition::fromBeats (4.0));
        REQUIRE (imported.size() == 1);
        vocal = imported[0];
        context.getSongSelection().clear();
        edit->getUndoManager().clearUndoHistory();

        phazite::addSelectionCommands (registry, getSelection (context), getRangeSelection (context));
        phazite::ui::addTimelineCommands (registry, [this] { return &editor; });
        phazite::test::dispatchPendingMessages();
    }

    ~EditorScreen() { setEdit (context, nullptr); }

    phazite::AudioClipInfo get() const
    {
        const auto info = phazite::getAudioClip (context, vocal);
        REQUIRE (info.has_value());
        return *info;
    }

    juce::String describeUndo() const { return phazite::describeUndo (edit.get()); }

    juce::Component& waveform() { return editor.getWaveformComponent(); }

    // Where a beat is across the waveform.
    int xOf (double beat) { return juce::roundToInt (editor.getView().beatToX (BeatPosition::fromBeats (beat))); }

    juce::Component& find (const juce::String& title)
    {
        auto* found = phazite::test::findComponentWithTitle (editor, title);
        REQUIRE (found != nullptr);
        return *found;
    }

    // The toolbar's text, which says which clip shows, where it is, and what it plays.
    juce::String getClipText()
    {
        for (auto* label : phazite::test::findAll<juce::Label> (editor))
            if (label->getText().contains ("of the song"))
                return label->getText();

        return {};
    }

    phazite::test::TemporaryFolder temp;
    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine()); // 120 BPM, 4/4
    te::AudioTrack* a = nullptr;
    te::AudioTrack* b = nullptr;
    phazite::ClipID midi, vocal;
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::AudioClipEditor editor { commands, context };
    phazite::test::TestWindow window { editor, 1000, 400 };
};
} // namespace

TEST_CASE ("The audio clip editor shows the selected audio clip, in the song's bars")
{
    EditorScreen screen;
    auto& selection = screen.context.getSongSelection();
    CHECK_FALSE (screen.editor.getClip().isValid());
    CHECK_FALSE (screen.find ("Gain").isEnabled());

    // A track that holds audio shows its clip at the playhead, or the nearest one.
    selection.add (phazite::getTrackID (*screen.b));
    phazite::test::dispatchPendingMessages();
    CHECK (screen.editor.getClip() == screen.vocal);

    // Selecting a clip in the playlist shows it, with all of it in view.
    screen.editor.showClip ({});
    CHECK_FALSE (screen.editor.getClip().isValid());
    selection.selectOnly (screen.vocal);
    phazite::test::dispatchPendingMessages();
    CHECK (screen.editor.getClip() == screen.vocal);
    CHECK (screen.xOf (4.0) > 0);
    CHECK (screen.xOf (12.0) < screen.waveform().getWidth());

    // Its controls show how it plays, and its text where it is and what it plays.
    CHECK (screen.getClipText().contains ("at bar 2 of the song"));
    CHECK (screen.getClipText().contains ("Vocal.wav"));
    CHECK (screen.find ("Gain").isEnabled());
    CHECK_FALSE (screen.find ("Clip tempo").isEnabled()); // until it follows the tempo

    // A MIDI clip selected leaves it showing (the dock shows the piano roll then).
    selection.selectOnly (screen.midi);
    phazite::test::dispatchPendingMessages();
    CHECK (screen.editor.getClip() == screen.vocal);

    // Once the clip is gone, nothing shows.
    phazite::deleteClips (screen.context, { screen.vocal }, false);
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (screen.editor.getClip().isValid());
    CHECK_FALSE (screen.find ("Gain").isEnabled());
}

TEST_CASE ("In the audio clip editor, the clip's edges trim it and its handles fade it, each drag one undo step")
{
    EditorScreen screen;
    screen.editor.showClip (screen.vocal);
    phazite::test::dispatchPendingMessages();
    auto& waveform = screen.waveform();
    phazite::test::Mouse mouse (waveform);
    using Handle = phazite::ui::AudioClipEditor::Handle;
    const auto middleY = waveform.getHeight() / 2;

    // The start's grip, dragged a beat and a bit later: on the grid.
    mouse.press (screen.editor.getHandleBounds (Handle::trimStart).getCentre());
    mouse.dragTo ({ screen.xOf (5.1), middleY });
    mouse.release();
    CHECK (screen.get().clip.range.getStart().inBeats() == doctest::Approx (5.0));
    CHECK (screen.get().content.getStart().inBeats() == doctest::Approx (4.0)); // the audio stays where it was
    CHECK (screen.describeUndo() == juce::String ("Undo Trim Clip Start"));

    // The end's, earlier.
    mouse.press (screen.editor.getHandleBounds (Handle::trimEnd).getCentre());
    mouse.dragTo ({ screen.xOf (10.9), middleY });
    mouse.release();
    CHECK (screen.get().clip.range.getEnd().inBeats() == doctest::Approx (11.0));
    CHECK (screen.describeUndo() == juce::String ("Undo Trim Clip End"));

    // Each drag was one step.
    phazite::undo (*screen.edit);
    CHECK (screen.get().clip.range.getEnd().inBeats() == doctest::Approx (12.0));
    CHECK (screen.get().clip.range.getStart().inBeats() == doctest::Approx (5.0));
    phazite::redo (*screen.edit);

    // The fade-in's handle, from the clip's top left corner to two beats in; a double-click on it takes the fade away.
    const auto fadeIn = screen.editor.getHandleBounds (Handle::fadeIn).getCentre();
    mouse.pause();
    mouse.press (fadeIn);
    mouse.dragTo ({ screen.xOf (7.0), fadeIn.y });
    mouse.release();
    CHECK (screen.get().clip.fadeIn == doctest::Approx (2.0 / 6.0).epsilon (0.01)); // of its six beats, to the pixel: fades aren't snapped
    CHECK (screen.describeUndo() == juce::String ("Undo Fade In"));

    mouse.pause();
    mouse.doubleClick (screen.editor.getHandleBounds (Handle::fadeIn).getCentre());
    CHECK (screen.get().clip.fadeIn == doctest::Approx (0.0));
    CHECK (screen.describeUndo() == juce::String ("Undo Remove Fade In"));

    // The fade-out's, a beat in from the end.
    const auto fadeOut = screen.editor.getHandleBounds (Handle::fadeOut).getCentre();
    mouse.pause();
    mouse.press (fadeOut);
    mouse.dragTo ({ screen.xOf (10.0), fadeOut.y });
    mouse.release();
    CHECK (screen.get().clip.fadeOut == doctest::Approx (1.0 / 6.0).epsilon (0.01));
    CHECK (screen.describeUndo() == juce::String ("Undo Fade Out"));

    // The audio, dragged a beat later inside the clip, which stays where it is.
    mouse.pause();
    mouse.press ({ screen.xOf (8.0), middleY });
    mouse.dragTo ({ screen.xOf (9.0), middleY });
    mouse.release();
    CHECK (screen.get().content.getStart().inBeats() == doctest::Approx (5.0));
    CHECK (screen.get().clip.range.getStart().inBeats() == doctest::Approx (5.0));
    CHECK (screen.describeUndo() == juce::String ("Undo Slip Clip"));
}

TEST_CASE ("In the audio clip editor, a vocal is turned up, pitched, reversed and stretched to the tempo, each change one undo step")
{
    EditorScreen screen;
    screen.editor.showClip (screen.vocal);
    phazite::test::dispatchPendingMessages();

    // Turned up: a drag up the gain field moves it half a decibel every two pixels.
    auto& gain = screen.find ("Gain");
    phazite::test::Mouse gainMouse (gain);
    const auto gainCentre = gain.getLocalBounds().getCentre();
    gainMouse.press (gainCentre);
    gainMouse.dragTo (gainCentre.translated (0, -6));
    gainMouse.dragTo (gainCentre.translated (0, -12));
    gainMouse.release();
    CHECK (screen.get().gain == doctest::Approx (3.0f));
    CHECK (screen.describeUndo() == juce::String ("Undo Change Clip Gain"));
    phazite::undo (*screen.edit); // the whole drag
    CHECK (screen.get().gain == doctest::Approx (0.0f));
    phazite::redo (*screen.edit);

    // Pitched: two semitones up, then 30 cents down.
    auto& pitch = screen.find ("Pitch");
    phazite::test::Mouse pitchMouse (pitch);
    const auto pitchCentre = pitch.getLocalBounds().getCentre();
    pitchMouse.press (pitchCentre);
    pitchMouse.dragTo (pitchCentre.translated (0, -4));
    pitchMouse.release();
    CHECK (screen.get().pitch == doctest::Approx (2.0f));
    CHECK (screen.describeUndo() == juce::String ("Undo Change Clip Pitch"));

    auto& fine = screen.find ("Fine tuning");
    phazite::test::Mouse fineMouse (fine);
    const auto fineCentre = fine.getLocalBounds().getCentre();
    fineMouse.press (fineCentre);
    fineMouse.dragTo (fineCentre.translated (0, 60));
    fineMouse.release();
    CHECK (screen.get().pitch == doctest::Approx (1.7f));
    CHECK (screen.get().clip.range.getEnd().inBeats() == doctest::Approx (12.0)); // the same length

    // Reversed, and forwards again.
    auto* reverse = dynamic_cast<juce::Button*> (&screen.find ("Reverse"));
    REQUIRE (reverse != nullptr);
    phazite::test::Mouse reverseMouse (*reverse);
    reverseMouse.click (reverse->getLocalBounds().getCentre());
    CHECK (screen.get().reversed);
    CHECK (screen.describeUndo() == juce::String ("Undo Reverse Clip"));
    reverseMouse.pause();
    reverseMouse.click (reverse->getLocalBounds().getCentre());
    CHECK_FALSE (screen.get().reversed);

    // Stretched to the tempo: it follows the song's, from the song's own, and then from the 60 BPM it was sung at, it plays
    // twice as fast.
    auto* follow = dynamic_cast<juce::Button*> (&screen.find ("Follow tempo"));
    REQUIRE (follow != nullptr);
    phazite::test::Mouse followMouse (*follow);
    followMouse.click (follow->getLocalBounds().getCentre());
    CHECK (screen.get().followsTempo);
    CHECK (screen.describeUndo() == juce::String ("Undo Follow Tempo"));
    phazite::test::dispatchPendingMessages();

    auto& tempo = screen.find ("Clip tempo");
    CHECK (tempo.isEnabled());
    phazite::test::Mouse tempoMouse (tempo);
    const auto tempoCentre = tempo.getLocalBounds().getCentre();
    tempoMouse.press (tempoCentre);
    tempoMouse.dragTo (tempoCentre.translated (0, 60));
    tempoMouse.dragTo (tempoCentre.translated (0, 120));
    tempoMouse.release();
    CHECK (screen.get().tempo == doctest::Approx (60.0));
    CHECK (screen.get().clip.range.getStart().inBeats() == doctest::Approx (4.0));
    CHECK (screen.get().clip.range.getEnd().inBeats() == doctest::Approx (8.0));
    CHECK (screen.describeUndo() == juce::String ("Undo Change Clip Tempo"));

    phazite::undo (*screen.edit); // the whole drag
    CHECK (screen.get().tempo == doctest::Approx (120.0));
    CHECK (screen.get().clip.range.getEnd().inBeats() == doctest::Approx (12.0));
}

TEST_CASE ("The editor dock's first tab shows the piano roll or the audio clip editor, as the selection asks, named for it")
{
    EditorScreen screen;
    phazite::ui::PianoRoll pianoRoll { screen.commands, screen.context };
    phazite::ui::AudioClipEditor audioEditor { screen.commands, screen.context };
    juce::Component devices;
    phazite::ui::EditorDock dock { screen.context, pianoRoll, audioEditor, devices };
    phazite::test::TestWindow dockWindow { dock, 1000, 400 };
    auto& selection = screen.context.getSongSelection();
    CHECK (dock.getTabNames()[phazite::ui::EditorDock::clipEditor] == juce::String ("Piano roll"));
    CHECK (pianoRoll.isVisible());
    CHECK_FALSE (audioEditor.isVisible());

    selection.selectOnly (screen.vocal);
    phazite::test::dispatchPendingMessages();
    CHECK (dock.showsAudioClip());
    CHECK (dock.getTabNames()[phazite::ui::EditorDock::clipEditor] == juce::String ("Audio clip"));
    CHECK (audioEditor.isVisible());
    CHECK_FALSE (pianoRoll.isVisible());
    CHECK (&dock.getClipEditor() == &audioEditor);
    CHECK (audioEditor.getClip() == screen.vocal);

    selection.selectOnly (screen.midi);
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (dock.showsAudioClip());
    CHECK (dock.getTabNames()[phazite::ui::EditorDock::clipEditor] == juce::String ("Piano roll"));
    CHECK (pianoRoll.isVisible());
    CHECK (&dock.getClipEditor() == &pianoRoll);

    // Opening a clip, as a double-click in the playlist does, shows it in its editor.
    selection.clear();
    phazite::test::dispatchPendingMessages();
    dock.openClip (screen.vocal);
    CHECK (dock.showsAudioClip());
    CHECK (audioEditor.getClip() == screen.vocal);
    dock.openClip (screen.midi);
    CHECK_FALSE (dock.showsAudioClip());
    CHECK (pianoRoll.getClips() == std::vector<phazite::ClipID> { screen.midi });
}
