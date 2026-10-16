// SPDX-License-Identifier: AGPL-3.0-or-later
// What the playlist's and the piano roll's tools share while the mouse is down (src/ui/timeline/TimelineGesture.h).
#include "TestHelpers.h"
#include "model/commands/Undo.h"
#include "model/project/ProjectContext.h"
#include "ui/timeline/TimelineGesture.h"

using phazite::BeatPosition;

TEST_CASE ("A gesture's changes are one undo step, the first one's, however many steps it begins")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    auto* track = te::getAudioTracks (*edit)[0];
    const auto name = track->getName();
    phazite::ProjectContext context { phazite::test::getEngine() }; // the open project, as the app has it
    context.setEdit (edit.get());
    edit->getUndoManager().clearUndoHistory();

    {
        phazite::ui::TimelineGesture gesture;
        gesture.beginStep (context, "Add Note");
        track->setName ("Added");
        gesture.beginStep (context, "Move Notes"); // the drag after the click
        track->setName ("Moved");
    }

    CHECK (edit->getUndoManager().getUndoDescription() == juce::String ("Add Note"));
    phazite::undo (*edit);
    CHECK (track->getName() == name);
    CHECK_FALSE (edit->getUndoManager().canUndo());
}

TEST_CASE ("A gesture does each thing once, and finds where it began after the content scrolled")
{
    phazite::ui::TimelineGesture gesture;
    const phazite::NoteRef first { phazite::ClipID::fromRaw (1), phazite::NoteID::fromRaw (1) };
    const phazite::NoteRef second { phazite::ClipID::fromRaw (1), phazite::NoteID::fromRaw (2) };
    CHECK (gesture.doOnce (first));
    CHECK_FALSE (gesture.doOnce (first));
    CHECK (gesture.doOnce (second));
    CHECK (gesture.doOnce (phazite::ClipID::fromRaw (1))); // a clip, not a note of it
    CHECK_FALSE (gesture.doOnce (phazite::ClipID::fromRaw (1)));

    phazite::ui::TimelineView view;
    view.pixelsPerBeat = 10.0;
    gesture.startBeat = BeatPosition::fromBeats (2.0);
    gesture.startPosition = { 20, 100 };
    gesture.startScrollY = 30;

    // The rows scrolled 20 pixels further down since: where the mouse went down is 20 pixels higher now.
    CHECK (gesture.getStart (view, 50) == juce::Point<int> (20, 80));

    // Unless it went down in a band at the top that doesn't scroll.
    CHECK (gesture.getStart (view, 50, 120) == juce::Point<int> (20, 100));

    // The time line scrolled sideways: the beat it went down on is where it is now.
    view.start = BeatPosition::fromBeats (1.0);
    CHECK (gesture.getStart (view, 50).x == 10);
}
