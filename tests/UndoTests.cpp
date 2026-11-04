// SPDX-License-Identifier: AGPL-3.0-or-later
// Undo, redo and the undo convention (src/model/commands/internal/Undo.h): a change of its own begins a named UndoStep, which
// joins a step already in progress for the same song, and a drag is a Gesture.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/internal/Project.h"

namespace
{
phazite::Song::Ptr newSong()
{
    auto song = phazite::createSong();
    song->getUndoManager().clearUndoHistory();
    return song;
}

// A change to the song, as the model makes one, to its own element.
void change (phazite::Song& song, const char* property, int value)
{
    song.getState().setProperty (property, value, &song.getUndoManager());
}
} // namespace

TEST_CASE ("An undo step begun inside another for the same song joins it")
{
    auto song = newSong();
    auto& undo = song->getUndoManager();

    SUBCASE ("Inside another for the same song: one step, named after the outer one")
    {
        {
            const phazite::UndoStep outer (*song, "Outer");
            change (*song, "a", 1);

            {
                const phazite::UndoStep inner (*song, "Inner");
                change (*song, "b", 2);
            }

            change (*song, "c", 3);
        }

        CHECK (undo.getUndoDescription() == juce::String ("Outer"));
        CHECK (undo.getNumActionsInCurrentTransaction() == 3);

        // One undo reverts all of it.
        phazite::undo (*song);
        CHECK_FALSE (song->getState().hasProperty ("a"));
        CHECK_FALSE (song->getState().hasProperty ("b"));
        CHECK_FALSE (song->getState().hasProperty ("c"));
        CHECK_FALSE (undo.canUndo());
    }

    SUBCASE ("One after the other: two named steps")
    {
        {
            const phazite::UndoStep first (*song, "First");
            change (*song, "a", 1);
        }

        {
            const phazite::UndoStep second (*song, "Second");
            change (*song, "b", 2);
        }

        CHECK (undo.getUndoDescription() == juce::String ("Second"));
        phazite::undo (*song);
        CHECK (song->getState().hasProperty ("a"));
        CHECK_FALSE (song->getState().hasProperty ("b"));
        CHECK (undo.getUndoDescription() == juce::String ("First"));
        phazite::undo (*song);
        CHECK_FALSE (song->getState().hasProperty ("a"));
        CHECK_FALSE (undo.canUndo());
    }

    SUBCASE ("Inside a step for another song: a step of its own")
    {
        auto other = newSong();

        {
            const phazite::UndoStep outer (*song, "Outer");
            change (*song, "a", 1);

            {
                const phazite::UndoStep inner (*other, "Inner");
                change (*other, "b", 2);

                // Inside that one again, a step for the first song still joins the outer one.
                const phazite::UndoStep innermost (*song, "Innermost");
                change (*song, "c", 3);
            }
        }

        CHECK (undo.getUndoDescription() == juce::String ("Outer"));
        CHECK (undo.getNumActionsInCurrentTransaction() == 2);
        CHECK (other->getUndoManager().getUndoDescription() == juce::String ("Inner"));

        phazite::undo (*other);
        CHECK_FALSE (other->getState().hasProperty ("b"));
        CHECK (song->getState().hasProperty ("a")); // the other song's undo leaves this one alone
        CHECK (song->getState().hasProperty ("c"));
    }
}

TEST_CASE ("Undo and redo say what they would undo")
{
    auto song = newSong();

    // Nothing to undo or redo: the bare verbs.
    CHECK_FALSE (phazite::canUndo (*song));
    CHECK_FALSE (phazite::canRedo (*song));
    CHECK (phazite::describeUndo (song.get()) == juce::String ("Undo"));
    CHECK (phazite::describeRedo (song.get()) == juce::String ("Redo"));
    CHECK (phazite::describeUndoOrRedo (song.get()) == juce::String ("Undo"));

    {
        const phazite::UndoStep step (*song, "Set Value");
        change (*song, "testValue", 1);
    }

    CHECK (phazite::canUndo (*song));
    CHECK_FALSE (phazite::canRedo (*song));
    CHECK (phazite::describeUndo (song.get()) == juce::String ("Undo Set Value"));
    CHECK (phazite::describeRedo (song.get()) == juce::String ("Redo"));
    CHECK (phazite::describeUndoOrRedo (song.get()) == juce::String ("Undo Set Value"));

    // FL Studio's undo-or-redo undoes, then redoes what it undid.
    phazite::undoOrRedo (*song);
    CHECK_FALSE (song->getState().hasProperty ("testValue"));
    CHECK_FALSE (phazite::canUndo (*song));
    CHECK (phazite::canRedo (*song));
    CHECK (phazite::describeUndo (song.get()) == juce::String ("Undo"));
    CHECK (phazite::describeRedo (song.get()) == juce::String ("Redo Set Value"));
    CHECK (phazite::describeUndoOrRedo (song.get()) == juce::String ("Redo Set Value"));

    phazite::undoOrRedo (*song);
    CHECK ((int) song->getState()["testValue"] == 1);
    CHECK (phazite::describeUndoOrRedo (song.get()) == juce::String ("Undo Set Value"));

    // Without a song.
    CHECK (phazite::describeUndo (nullptr) == juce::String ("Undo"));
    CHECK (phazite::describeRedo (nullptr) == juce::String ("Redo"));
    CHECK (phazite::describeUndoOrRedo (nullptr) == juce::String ("Undo"));

    // An unnamed step: the bare verb.
    song->getUndoManager().beginNewTransaction();
    change (*song, "testValue", 2);
    CHECK (phazite::canUndo (*song));
    CHECK (phazite::describeUndo (song.get()) == juce::String ("Undo"));
    CHECK (phazite::describeUndoOrRedo (song.get()) == juce::String ("Undo"));
}

TEST_CASE ("An undo that puts back what devices play keeps what it undid to redo")
{
    auto song = newSong();
    const auto track = phazite::ensureTracks (*song, 1).front();

    SUBCASE ("A fader turned on a track the step added, which the undo takes away")
    {
        {
            const phazite::UndoStep step (*song, "Add and Turn");
            const auto added = phazite::insertPlainTrack (*song, track);
            REQUIRE (added.isValid());
            phazite::setChannelVolume (*song, added, -6.0f);
        }

        phazite::undo (*song);
        CHECK (phazite::ensureTracks (*song, 1).size() == 1);
        REQUIRE (phazite::canRedo (*song));
        phazite::redo (*song);
        const auto tracks = phazite::ensureTracks (*song, 1);
        REQUIRE (tracks.size() == 2);
        CHECK (phazite::getChannelVolume (*song, tracks[1]) == doctest::Approx (-6.0f).epsilon (0.01));
    }

    SUBCASE ("A fader turned for the first time")
    {
        const auto before = phazite::getChannelVolume (*song, track);

        {
            const phazite::UndoStep step (*song, "Turn");
            phazite::setChannelVolume (*song, track, -6.0f);
        }

        phazite::undo (*song);
        CHECK (phazite::getChannelVolume (*song, track) == doctest::Approx (before));
        REQUIRE (phazite::canRedo (*song));
        phazite::redo (*song);
        CHECK (phazite::getChannelVolume (*song, track) == doctest::Approx (-6.0f).epsilon (0.01));
    }
}

TEST_CASE ("A gesture is one named undo step until it ends, and rolling it back leaves nothing to undo or redo")
{
    auto song = newSong();
    auto& undo = song->getUndoManager();

    {
        phazite::Gesture gesture (*song, "Drag");
        CHECK (gesture.isOpen());
        change (*song, "a", 1);

        // A gesture keeps its step whole however long it lasts, across turns of the message loop.
        phazite::test::settle (500);
        change (*song, "a", 2);

        {
            const phazite::UndoStep inside (*song, "Inside"); // a model function's step, which joins the gesture
            change (*song, "b", 3);
        }

        change (*song, "a", 4);
        gesture.commit();
        CHECK_FALSE (gesture.isOpen());
    }

    CHECK (undo.getUndoDescription() == juce::String ("Drag"));
    phazite::undo (*song);
    CHECK_FALSE (song->getState().hasProperty ("a"));
    CHECK_FALSE (song->getState().hasProperty ("b"));
    phazite::redo (*song);
    CHECK ((int) song->getState()["a"] == 4);
    CHECK ((int) song->getState()["b"] == 3);

    // Rolled back, as Escape during a drag: as if it never happened, with nothing to redo.
    {
        phazite::Gesture gesture (*song, "Cancelled");
        change (*song, "a", 5);
        change (*song, "c", 6);
        gesture.rollBack();
        CHECK_FALSE (gesture.isOpen());
    }

    CHECK ((int) song->getState()["a"] == 4);
    CHECK_FALSE (song->getState().hasProperty ("c"));
    CHECK (undo.getUndoDescription() == juce::String ("Drag"));
    CHECK_FALSE (phazite::canRedo (*song));

    // Ended without either, it keeps its changes.
    {
        const phazite::Gesture gesture (*song, "Kept");
        change (*song, "d", 7);
    }

    CHECK (undo.getUndoDescription() == juce::String ("Kept"));
    CHECK ((int) song->getState()["d"] == 7);
}
