// SPDX-License-Identifier: AGPL-3.0-or-later
// What the playlist's and the piano roll's tools share while the mouse is down (src/ui/timeline/TimelineGesture.h).
#include "TestHelpers.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/timeline/TimelineGesture.h"

using phazite::BeatPosition;

TEST_CASE ("A gesture's changes are one undo step, the first one's, however many steps it begins")
{
    auto song = phazite::createSong();
    const auto track = phazite::getTrackIDs (*song).front();
    const auto name = phazite::getTrackName (*song, track);
    phazite::ProjectContext context { phazite::test::getEngineContext() }; // the open project, as the app has it
    setSong (context, song.get());
    song->getUndoManager().clearUndoHistory();

    {
        phazite::ui::TimelineGesture gesture;
        gesture.beginStep (context, "Add Note");
        phazite::setTrackName (*song, track, "Added");
        gesture.beginStep (context, "Move Notes"); // the drag after the click
        phazite::setTrackName (*song, track, "Moved");
    }

    CHECK (song->getUndoManager().getUndoDescription() == juce::String ("Add Note"));
    phazite::undo (*song);
    CHECK (phazite::getTrackName (*song, track) == name);
    CHECK_FALSE (song->getUndoManager().canUndo());
    setSong (context, nullptr);
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
