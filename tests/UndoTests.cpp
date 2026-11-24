// SPDX-License-Identifier: AGPL-3.0-or-later
// Undo, redo and the undo convention (src/model/commands/internal/Undo.h): a change of its own begins a named UndoStep, which
// joins a step already in progress for the same song, and a drag is a Gesture. And the check that Undo and Redo are exact
// (UndoCheck.h): what it finds and what it leaves out, and the song's changes it guards beyond the commands' script
// (tests/CommandGoldenTests.cpp).
#include "GoldenFiles.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "TestSongOwn.h"
#include "TestTracks.h"
#include "UndoCheck.h"
#include "devices/reverb/Reverb.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/commands/UndoHistory.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Freeze.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/NotesModel.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Metronome.h"
#include "model/transport/internal/Playback.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace
{
phazite::Song::Ptr newSong()
{
    auto song = phazite::createSong();
    song->getUndoManager().clearUndoHistory();
    return song;
}

// A song with four tracks, a counted value each in its history (TestSongOwn.h: setTrackProbe()): for the tests that need
// several independent changes, which change() makes, by the probe's letter, "a" to "d", and isSet() and valueOf() read.
phazite::Song::Ptr newSongWithProbes()
{
    auto song = phazite::createSong();
    phazite::ensureTracks (*song, 4);
    song->getUndoManager().clearUndoHistory();
    return song;
}

int probePlace (const char* probe)
{
    const auto place = juce::String ("abcd").indexOf (juce::String (probe));
    REQUIRE (place >= 0);
    return place;
}

// A change to the song, as the model makes one, joining the step in progress.
void change (phazite::Song& song, const char* probe, int value)
{
    phazite::test::setTrackProbe (song, probePlace (probe), value);
}

int valueOf (const phazite::Song& song, const char* probe)
{
    return phazite::test::getTrackProbe (song, probePlace (probe));
}

bool isSet (const phazite::Song& song, const char* probe)
{
    return valueOf (song, probe) != -1;
}
} // namespace

TEST_CASE ("An undo step begun inside another for the same song joins it")
{
    auto song = newSongWithProbes();
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
        CHECK (undo.getUndoDescriptions().size() == 1);

        // One undo reverts all of it.
        phazite::undo (*song);
        CHECK_FALSE (isSet (*song, "a"));
        CHECK_FALSE (isSet (*song, "b"));
        CHECK_FALSE (isSet (*song, "c"));
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
        CHECK (isSet (*song, "a"));
        CHECK_FALSE (isSet (*song, "b"));
        CHECK (undo.getUndoDescription() == juce::String ("First"));
        phazite::undo (*song);
        CHECK_FALSE (isSet (*song, "a"));
        CHECK_FALSE (undo.canUndo());
    }

    SUBCASE ("Inside a step for another song: a step of its own")
    {
        auto other = newSongWithProbes();

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
        CHECK (undo.getUndoDescriptions().size() == 1);
        CHECK (other->getUndoManager().getUndoDescription() == juce::String ("Inner"));
        CHECK (other->getUndoManager().getUndoDescriptions().size() == 1);

        phazite::undo (*other);
        CHECK_FALSE (isSet (*other, "b"));
        CHECK (isSet (*song, "a")); // the other song's undo leaves this one alone
        CHECK (isSet (*song, "c"));
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
        phazite::test::setStepProbe (*song, "1");
    }

    CHECK (phazite::canUndo (*song));
    CHECK_FALSE (phazite::canRedo (*song));
    CHECK (phazite::describeUndo (song.get()) == juce::String ("Undo Set Value"));
    CHECK (phazite::describeRedo (song.get()) == juce::String ("Redo"));
    CHECK (phazite::describeUndoOrRedo (song.get()) == juce::String ("Undo Set Value"));

    // FL Studio's undo-or-redo undoes, then redoes what it undid.
    phazite::undoOrRedo (*song);
    CHECK (phazite::test::getStepProbe (*song).isEmpty());
    CHECK_FALSE (phazite::canUndo (*song));
    CHECK (phazite::canRedo (*song));
    CHECK (phazite::describeUndo (song.get()) == juce::String ("Undo"));
    CHECK (phazite::describeRedo (song.get()) == juce::String ("Redo Set Value"));
    CHECK (phazite::describeUndoOrRedo (song.get()) == juce::String ("Redo Set Value"));

    phazite::undoOrRedo (*song);
    CHECK (phazite::test::getStepProbe (*song) == "1");
    CHECK (phazite::describeUndoOrRedo (song.get()) == juce::String ("Undo Set Value"));

    // Without a song.
    CHECK (phazite::describeUndo (nullptr) == juce::String ("Undo"));
    CHECK (phazite::describeRedo (nullptr) == juce::String ("Redo"));
    CHECK (phazite::describeUndoOrRedo (nullptr) == juce::String ("Undo"));

    // An unnamed step: the bare verb.
    song->getUndoManager().beginNewTransaction();
    phazite::test::setStepProbe (*song, "2");
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
    auto song = newSongWithProbes();
    auto& undo = song->getUndoManager();

    {
        phazite::Gesture gesture (*song, "Drag");
        CHECK (gesture.isOpen());
        change (*song, "a", 1);

        // A gesture keeps its step whole however long it lasts, across turns of the message loop.
        phazite::test::dispatchPendingMessages(); // nothing of a gesture reads the clock: the turns are what count
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
    CHECK_FALSE (isSet (*song, "a"));
    CHECK_FALSE (isSet (*song, "b"));
    phazite::redo (*song);
    CHECK (valueOf (*song, "a") == 4);
    CHECK (valueOf (*song, "b") == 3);

    // Rolled back, as Escape during a drag: as if it never happened, with nothing to redo.
    {
        phazite::Gesture gesture (*song, "Cancelled");
        change (*song, "a", 5);
        change (*song, "c", 6);
        gesture.rollBack();
        CHECK_FALSE (gesture.isOpen());
    }

    CHECK (valueOf (*song, "a") == 4);
    CHECK_FALSE (isSet (*song, "c"));
    CHECK (undo.getUndoDescription() == juce::String ("Drag"));
    CHECK_FALSE (phazite::canRedo (*song));

    // Ended without either, it keeps its changes.
    {
        const phazite::Gesture gesture (*song, "Kept");
        change (*song, "d", 7);
    }

    CHECK (undo.getUndoDescription() == juce::String ("Kept"));
    CHECK (valueOf (*song, "d") == 7);
}

TEST_CASE ("The undo history lists the song's steps by name, oldest first, and goes back or forth to one as Undo and Redo do")
{
    phazite::test::TestSong song;
    auto& context = song.context;
    const auto countTracks = [&context] { return phazite::getPlaylistRows (context).size(); };
    const auto tracksBefore = countTracks();
    const auto before = phazite::getUndoHistory (context);
    CHECK (before.done == (int) before.steps.size());

    for (int i = 0; i < 3; ++i)
        REQUIRE (phazite::addTrackAtEnd (context, false).isValid());

    auto history = phazite::getUndoHistory (context);
    const auto all = (int) history.steps.size();
    REQUIRE (all == (int) before.steps.size() + 3);
    CHECK (history.done == all);
    CHECK (history.steps.back().isNotEmpty());
    CHECK (history.steps.back() == phazite::describeUndo (phazite::findSong (context)).fromFirstOccurrenceOf ("Undo ", false, false));

    // Back two steps: two tracks fewer, the steps still listed, undone, for Redo.
    phazite::goToUndoStep (context, all - 2);
    CHECK (countTracks() == tracksBefore + 1);
    history = phazite::getUndoHistory (context);
    CHECK ((int) history.steps.size() == all);
    CHECK (history.done == all - 2);
    CHECK (phazite::canRedo (*phazite::findSong (context)));

    // Forth again; and past either end, to that end.
    phazite::goToUndoStep (context, all);
    CHECK (countTracks() == tracksBefore + 3);
    phazite::goToUndoStep (context, -5);
    CHECK (phazite::getUndoHistory (context).done == 0);
    phazite::goToUndoStep (context, all + 5);
    CHECK (phazite::getUndoHistory (context).done == all);
    CHECK (countTracks() == tracksBefore + 3);

    // A change after going back leaves the steps undone behind, as any change after Undo does.
    phazite::goToUndoStep (context, all - 1);
    REQUIRE (phazite::addTrackAtEnd (context, false).isValid());
    history = phazite::getUndoHistory (context);
    CHECK ((int) history.steps.size() == all);
    CHECK (history.done == all);

    // Without a project: none, and going to a step does nothing.
    phazite::ProjectContext none { phazite::test::getEngineContext() };
    CHECK (phazite::getUndoHistory (none).steps.empty());
    phazite::goToUndoStep (none, 3);
}

//==============================================================================
// The check that Undo and Redo are exact (UndoCheck.h).
namespace
{
// A check whose failures are kept rather than failing the test, for the tests of what it finds.
struct KeptFailures
{
    explicit KeptFailures (phazite::Song& song)
        : check (song)
    {
        check.onFailure = [this] (const juce::String& failure) { failures.add (failure); };
    }

    phazite::test::UndoCheck check;
    juce::StringArray failures;
};
} // namespace

TEST_CASE ("The undo check passes a change that is one exact undo step, and one that changes only what no undo step keeps")
{
    auto song = newSong();
    const auto track = phazite::ensureTracks (*song, 1).front();
    phazite::test::UndoCheck check (*song);

    check.check ("Set Value",
                 [&song]
                 {
                     const phazite::UndoStep step (*song, "Set Value");
                     phazite::test::setStepProbe (*song, "1");
                 });
    CHECK (check.getNumSteps() == 1);
    CHECK (phazite::test::getStepProbe (*song) == "1"); // the song is left as the change left it
    CHECK (song->getUndoManager().getUndoDescription() == juce::String ("Set Value"));

    // A switch that is no undo step, a view setting, and how the song plays: no step, and nothing to undo.
    check.check ("Mute", [&song, track] { phazite::setTrackMuted (*song, track, true, nullptr); });
    check.check ("Collapse", [&song, track] { phazite::setCollapsed (*song, track, true); });
    check.check ("Metronome", [&song] { phazite::setMetronomeOn (*song, true); });
    CHECK (check.getNumSteps() == 1);

    // The test's own changes, with the model's functions that join their caller's step: one step, or several.
    check.checkSeveral ("a track, named and turned down",
                        [&song, track]
                        {
                            const auto added = phazite::insertPlainTrack (*song, track);
                            REQUIRE (added.isValid());
                            phazite::setTrackName (*song, added, "Bass");
                            phazite::setChannelVolume (*song, added, -6.0f);
                        });
    CHECK (check.getNumSteps() >= 2);

    // An undo and a redo that the test runs itself.
    check.stepBackOrForth ("Undo", [&song] { phazite::undo (*song); });
    CHECK (phazite::ensureTracks (*song, 1).size() == 1);
    check.stepBackOrForth ("Redo", [&song] { phazite::redo (*song); });
    CHECK (phazite::ensureTracks (*song, 1).size() == 2);

    // Back through every step and forth: the song at each as it was, and at the end all of it, the mute too.
    check.walkBackAndForth();
    CHECK (phazite::ensureTracks (*song, 1).size() == 2);
    CHECK ((bool) phazite::test::getTrackProperty (*song, track, phazite::schema::trackMute));
}

TEST_CASE ("The undo check fails a change that Undo doesn't take back whole, and what makes a later Undo take the wrong thing")
{
    auto song = newSongWithProbes();

    SUBCASE ("A part of the change made round the undo history: Undo leaves it")
    {
        KeptFailures kept (*song);
        kept.check.check ("Half a Step",
                          [&song]
                          {
                              const phazite::UndoStep step (*song, "Half a Step");
                              change (*song, "a", 1);
                              phazite::test::changeRoundTheHistory (*song);
                          });

        REQUIRE (kept.failures.size() == 1);
        CHECK (kept.failures[0].startsWith ("Half a Step: Undo doesn't give the song as it was before:"));
        CHECK (kept.failures[0].contains ("SECTION"));
        CHECK_FALSE (kept.failures[0].contains ("TRACK"));
    }

    SUBCASE ("A change without a step of its own, to what undo steps keep")
    {
        KeptFailures kept (*song);
        kept.check.check ("No Step", [&song] { phazite::test::changeRoundTheHistory (*song); });

        REQUIRE (kept.failures.size() == 1);
        CHECK (kept.failures[0].startsWith ("No Step: changed what undo steps keep without an undo step of its own"));
        CHECK (kept.failures[0].contains ("SECTION"));
    }

    SUBCASE ("A change that joins the step before")
    {
        // A change through the history that begins no step joins the one in progress: the check sees no step of its own.
        song->getUndoManager().beginNewTransaction ("First");
        change (*song, "a", 1);
        KeptFailures kept (*song);
        kept.check.check ("Joins", [&song] { change (*song, "b", 2); });

        REQUIRE (kept.failures.size() == 1);
        CHECK (kept.failures[0].startsWith ("Joins: changed what undo steps keep without an undo step of its own"));

        // After a change the check has undone and redone, no step is in progress: the change is a step without a name.
        kept.check.check ("Second",
                          [&song]
                          {
                              const phazite::UndoStep step (*song, "Second");
                              change (*song, "c", 3);
                          });
        kept.check.check ("Joins Again", [&song] { change (*song, "d", 4); });

        REQUIRE (kept.failures.size() == 2);
        CHECK (kept.failures[1].startsWith ("Joins Again: made an undo step without a name"));
    }

    SUBCASE ("Two steps where a change is one")
    {
        KeptFailures kept (*song);
        kept.check.check ("Twice",
                          [&song]
                          {
                              {
                                  const phazite::UndoStep one (*song, "One");
                                  change (*song, "a", 1);
                              }

                              const phazite::UndoStep two (*song, "Two");
                              change (*song, "b", 2);
                          });

        REQUIRE (kept.failures.size() == 1);
        CHECK (kept.failures[0].startsWith ("Twice: made 2 undo steps"));
    }

    SUBCASE ("A switch that is no undo step, changed round the history by a change that is one")
    {
        KeptFailures kept (*song);
        const auto track = phazite::ensureTracks (*song, 1).front();
        kept.check.check ("Rename and Mute",
                          [&song, track]
                          {
                              const phazite::UndoStep step (*song, "Rename and Mute");
                              phazite::setTrackName (*song, track, "Lead");
                              phazite::setTrackMuted (*song, track, true, nullptr);
                          });

        REQUIRE (kept.failures.size() == 1);
        CHECK (kept.failures[0].startsWith ("Rename and Mute: Undo doesn't give the song as it was before:"));
        CHECK (kept.failures[0].contains ("@mute"));
    }

    SUBCASE ("A change round the history between two steps, to an area the step before changed: the walk's Undo gives another song")
    {
        // A plain area's Undo and Redo put back the whole value their step held (ValueArea.h): a section added round the
        // history after a step that added one is lost by that step's Undo and Redo (here the check's own, catching up as the
        // next change begins), so that the walk's Undo to the step between gives a song without the section it had there.
        KeptFailures kept (*song);
        const auto addSection = [&song] (const char* name)
        {
            REQUIRE (phazite::insertSection (*song, phazite::getSectionsEnd (*song),
                                             { .name = name, .length = phazite::BeatDuration::fromBeats (4.0) })
                         .isValid());
        };

        kept.check.check ("First",
                          [&song, &addSection]
                          {
                              const phazite::UndoStep step (*song, "First");
                              addSection ("First");
                          });
        phazite::test::changeRoundTheHistory (*song);
        kept.check.check ("Second",
                          [&song]
                          {
                              const phazite::UndoStep step (*song, "Second");
                              change (*song, "a", 1);
                          });
        CHECK (kept.failures.isEmpty());

        kept.check.walkBackAndForth();
        REQUIRE (! kept.failures.isEmpty());
        CHECK (kept.failures[0].startsWith ("The walk: Undo to step 1 of 2"));
        CHECK (kept.failures[0].contains ("doesn't give the song as it was there"));
        CHECK (kept.failures[0].contains ("SECTION"));
    }

    SUBCASE ("An undo that the check wasn't told of")
    {
        KeptFailures kept (*song);
        kept.check.check ("Set Value",
                          [&song]
                          {
                              const phazite::UndoStep step (*song, "Set Value");
                              change (*song, "a", 1);
                          });
        kept.check.check ("Undo", [&song] { phazite::undo (*song); });

        REQUIRE (kept.failures.size() == 1);
        CHECK (kept.failures[0].startsWith ("Undo: the song's undo history went back"));
    }
}

TEST_CASE ("The undo check's texts leave out what no undo step keeps, and nothing else")
{
    using phazite::test::SongPart;
    using phazite::test::writeSong;
    phazite::test::TemporaryFolder temp;
    auto song = newSong();
    const auto track = phazite::ensureTracks (*song, 1).front();
    REQUIRE (phazite::test::hasTrack (*song, track));

    const auto whole = writeSong (*song, SongPart::whole);
    const auto undoable = writeSong (*song, SongPart::undoable);
    const auto withoutSwitches = writeSong (*song, SongPart::undoableWithoutSwitches);
    CHECK (whole.contains ("nextID=")); // the count of IDs, which an undo never takes back
    CHECK_FALSE (undoable.contains ("nextID="));

    // View settings, how the song plays and what the project remembers outside the history: in the whole song only.
    phazite::setSavedTrackHeight (*song, track, 96);
    phazite::showLane (*song, track, { phazite::getFader (*song, track), phazite::getVolumeParameterID (*song, track) });
    phazite::toggleLoop (*song);
    REQUIRE (phazite::isLoopOn (*song));
    phazite::setLastKey (*song, phazite::Key { 2, phazite::Scale::major });
    phazite::freezeFromFile (*song, track, phazite::test::writeSound (temp.folder.getChildFile ("Frozen.wav"), 0.1));
    REQUIRE ((bool) phazite::test::getTrackProperty (*song, track, phazite::schema::trackFrozen));
    CHECK (writeSong (*song, SongPart::whole) != whole);
    CHECK (writeSong (*song, SongPart::undoable) == undoable);
    CHECK (writeSong (*song, SongPart::undoableWithoutSwitches) == withoutSwitches);

    // The switches that are no undo steps: compared around one change, left out between steps.
    phazite::setTrackMuted (*song, track, true);
    phazite::setTrackSoloed (*song, track, true);
    phazite::setAutomationMode (*song, track, phazite::AutomationMode::touch);
    CHECK (writeSong (*song, SongPart::undoable) != undoable);
    CHECK (writeSong (*song, SongPart::undoableWithoutSwitches) == withoutSwitches);

    // Anything else is the song's: a name, and the order of a parent's elements.
    {
        const phazite::UndoStep step (*song, "Rename Track");
        phazite::setTrackName (*song, track, "Lead");
    }

    CHECK (writeSong (*song, SongPart::undoableWithoutSwitches) != withoutSwitches);

    const auto second = phazite::insertPlainTrack (*song, track);
    REQUIRE (second.isValid());
    const auto inOrder = writeSong (*song, SongPart::undoableWithoutSwitches);

    phazite::moveTracksAfter (*song, { track }, second);
    REQUIRE (phazite::getTrackIDs (*song) == std::vector { second, track });
    const auto swapped = writeSong (*song, SongPart::undoableWithoutSwitches);
    CHECK (swapped != inOrder);
    CHECK_FALSE (phazite::test::describeSongDifferences (inOrder, swapped).isEmpty());
    CHECK (phazite::test::describeSongDifferences (inOrder, inOrder).isEmpty());
}

TEST_CASE ("Automation lanes shown and removed between undo steps leave each step's undo and redo its own clip")
{
    // Until 2026-11-13 a track's lanes were among its elements, added and removed outside the undo history, which finds an
    // element by its place: after a lane went, the undo of a clip added after it took another clip, or none.
    phazite::test::TestSong song;
    const auto track = song.firstTrack;
    const auto volume = song.getVolumeParameter (track);
    const auto pan = song.getPanParameter (track);
    phazite::test::UndoCheck check (song.context);
    const auto countClips = [&song, track] { return phazite::getClips (song.context, track).size(); };

    check.checkSeveral ("a clip in bar 1", [&song, track] { song.addMidiClip (track, 0.0, 4.0); });
    phazite::showAutomationLane (song.context, volume);
    phazite::showAutomationLane (song.context, pan);
    CHECK (phazite::getAutomationLanes (song.context, track).size() == 2);
    check.checkSeveral ("a clip in bar 2", [&song, track] { song.addMidiClip (track, 4.0, 4.0); });
    phazite::removeAutomationLane (song.context, volume);
    check.checkSeveral ("a clip in bar 3", [&song, track] { song.addMidiClip (track, 8.0, 4.0); });
    check.check ("Add Effect", [&song, track] { song.addEffect (track, phazite::devices::Reverb::typeID); });
    phazite::removeAutomationLane (song.context, pan);
    CHECK (phazite::getAutomationLanes (song.context, track).empty());
    REQUIRE (countClips() == 3);

    // Every step back, the song at each as it was there, and forth again.
    check.walkBackAndForth();
    CHECK (countClips() == 3);

    // And as someone does it: Undo takes the effect, then the last clip, whole, and Redo brings each back.
    auto& songState = *phazite::findSong (song.context);
    check.stepBackOrForth ("Undo Add Effect", [&songState] { phazite::undo (songState); });
    CHECK (phazite::getEffects (song.context, track).empty());
    CHECK (countClips() == 3);

    while (countClips() == 3 && phazite::canUndo (songState))
        check.stepBackOrForth ("Undo the clip in bar 3", [&songState] { phazite::undo (songState); });

    const auto clips = phazite::getClips (song.context, track);
    REQUIRE (clips.size() == 2);
    CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (clips[1].range.getStart().inBeats() == doctest::Approx (4.0));

    while (phazite::canRedo (songState))
        check.stepBackOrForth ("Redo", [&songState] { phazite::redo (songState); });

    CHECK (countClips() == 3);
    CHECK (phazite::getEffects (song.context, track).size() == 1);
}

TEST_CASE ("The undo history keeps the steps its setting says, however much each step changes")
{
    // A step that changes the song many times, as a drag of many notes does: JUCE's budget of units kept only 30 such steps.
    // Two values in turn, so that no change joins the one before it (JUCE's coalescing of a property set twice): 600 actions
    // a step while a track's name and the outline are the tree's properties. Once both are values a step holds one action
    // for each area it changed, however much it changed, and only "Set lower" still tells a budget of units from a count of
    // steps.
    const auto bigStep = [] (phazite::Song& song, int step)
    {
        const phazite::UndoStep undoStep (song, "Step " + juce::String (step));

        for (int i = 0; i < 300; ++i)
        {
            phazite::test::setTrackProbe (song, 0, step * 1000 + i);
            phazite::test::setStepProbe (song, juce::String (step) + "/" + juce::String (i));
        }
    };

    SUBCASE ("By default: hundreds of steps")
    {
        CHECK (phazite::Song::getUndoSteps() == phazite::Song::defaultUndoSteps);
        auto song = newSong();

        for (int step = 1; step <= 100; ++step)
            bigStep (*song, step);

        auto undone = 0;

        while (phazite::canUndo (*song))
        {
            phazite::undo (*song);
            ++undone;
        }

        CHECK (undone == 100);
        CHECK (phazite::test::getStepProbe (*song).isEmpty());
        CHECK (phazite::test::getTrackProbe (*song, 0) == -1);
    }

    SUBCASE ("Set lower: the oldest steps go")
    {
        phazite::Song::setUndoSteps (20);
        auto song = newSong();
        phazite::Song::setUndoSteps (phazite::Song::defaultUndoSteps);

        for (int step = 1; step <= 25; ++step)
            bigStep (*song, step);

        auto undone = 0;

        while (phazite::canUndo (*song))
        {
            phazite::undo (*song);
            ++undone;
        }

        CHECK (undone == 20);
        CHECK (phazite::test::getStepProbe (*song) == "5/299");
        CHECK (phazite::test::getTrackProbe (*song, 0) == 5299);
    }

    SUBCASE ("Kept within its range")
    {
        phazite::Song::setUndoSteps (0);
        CHECK (phazite::Song::getUndoSteps() == phazite::Song::minimumUndoSteps);
        phazite::Song::setUndoSteps (phazite::Song::defaultUndoSteps);
    }
}

TEST_CASE ("Two trials of a song's history never share a step: a wheel's burst and a Swap tried meanwhile are each a step of their "
           "own")
{
    // Begun alongside each other, each counted the other's changes as its own: the burst's next moves went into the Swap's step,
    // which putting the Swap back took away with them, and a Swap begun while a burst had changes stopped on an assertion
    // (a reviewer, 2026-11-14). Now a trial beginning or trying something keeps another's first, as a step beginning does.
    phazite::test::TestSong song;
    const auto clip = song.addMidiClip (song.firstTrack, 0.0, 4.0);
    const auto note = phazite::addNote (song.context, clip, 60, phazite::BeatPosition(), phazite::BeatDuration::fromBeats (1.0), 60);
    REQUIRE (note.isValid());
    auto& target = *phazite::findSong (song.context);
    target.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck undo (target);
    const auto velocity = [&] { return phazite::getNote (song.context, note).value().velocity; };
    const auto tryName = [&] (phazite::Trial& trial, const juce::String& name)
    { trial.apply ([&] { phazite::setTrackName (target, song.firstTrack, name); }, true); };
    const auto history = [&] { return target.getUndoManager().getUndoDescriptions().joinIntoString (", ").toStdString(); };

    SUBCASE ("A Swap open with nothing tried, a burst, a try, and the burst going on")
    {
        undo.checkSeveral ("a burst and a try, taking turns",
                           [&]
                           {
                               phazite::Trial swap (target, "Swap Preset", nullptr);
                               phazite::VelocityWheel wheel (song.context);
                               wheel.change ({ note }, 4);
                               tryName (swap, "Tried");    // keeps the burst first
                               wheel.change ({ note }, 4); // keeps the try first
                               wheel.end();
                               swap.putBack(); // kept already: nothing to put back
                           });

        CHECK (velocity() == 68);
        CHECK (phazite::getTrackName (target, song.firstTrack) == "Tried");
        CHECK (history() == "Change Velocity, Swap Preset, Change Velocity");
    }

    SUBCASE ("A burst with changes, then a Swap begun, tried and put back")
    {
        const auto nameBefore = phazite::getTrackName (target, song.firstTrack);
        undo.checkSeveral ("a burst, then a Swap",
                           [&]
                           {
                               phazite::VelocityWheel wheel (song.context);
                               wheel.change ({ note }, 4);
                               phazite::Trial swap (target, "Swap Preset", nullptr); // keeps the burst first
                               tryName (swap, "Tried");
                               swap.putBack();
                           });

        CHECK (velocity() == 64);
        CHECK (phazite::getTrackName (target, song.firstTrack) == nameBefore);
        CHECK (history() == "Change Velocity");
    }

    undo.walkBackAndForth();
}

TEST_CASE ("What waits for a gesture's end runs after it: committed, rolled back or destroyed, each once and in order, unless forgotten")
{
    // A reviewer, 2026-11-15: Gesture::whenEnded() had no test of its own. What a take or the automation writer keeps while a
    // drag is under way waits for it (model/transport/internal/Playback.h, stopPlaying()).
    auto song = newSong();
    std::vector<juce::String> ran;
    const auto note = [&ran] (const char* what) { return [&ran, what] { ran.push_back (what); }; };
    int owner = 0, other = 0;

    SUBCASE ("No gesture: at once")
    {
        phazite::Gesture::whenEnded (*song, &owner, note ("now"));
        CHECK (ran == std::vector<juce::String> { "now" });
    }

    SUBCASE ("Committed: after it, in order, outside its step")
    {
        {
            phazite::Gesture gesture (*song, "Drag");
            phazite::Gesture::whenEnded (*song, &owner, note ("first"));
            phazite::Gesture::whenEnded (*song, &other, note ("second"));
            CHECK (ran.empty());
            gesture.commit();
            CHECK (ran == std::vector<juce::String> { "first", "second" });
        }

        CHECK (ran.size() == 2); // once
    }

    SUBCASE ("Rolled back: after the rollback, its change a step of its own that the rollback kept")
    {
        phazite::Gesture gesture (*song, "Drag");
        phazite::test::setStepProbe (*song, "dragged");
        phazite::Gesture::whenEnded (*song, &owner,
                                     [&song]
                                     {
                                         const phazite::UndoStep step (*song, "Kept");
                                         phazite::test::setTrackProbe (*song, 0, 1);
                                     });
        gesture.rollBack();
        CHECK (phazite::test::getStepProbe (*song).isEmpty());
        CHECK (phazite::test::getTrackProbe (*song, 0) == 1);
        CHECK (song->getUndoManager().getUndoDescription() == juce::String ("Kept"));
    }

    SUBCASE ("Destroyed: as it commits")
    {
        {
            phazite::Gesture gesture (*song, "Drag");
            phazite::Gesture::whenEnded (*song, &owner, note ("after"));
        }

        CHECK (ran == std::vector<juce::String> { "after" });
    }

    SUBCASE ("Forgotten by its owner: never; another owner's still")
    {
        phazite::Gesture gesture (*song, "Drag");
        phazite::Gesture::whenEnded (*song, &owner, note ("forgotten"));
        phazite::Gesture::whenEnded (*song, &other, note ("kept"));
        phazite::Gesture::forget (&owner);
        gesture.commit();
        CHECK (ran == std::vector<juce::String> { "kept" });
    }

    SUBCASE ("A change that asks again: at once once the gesture has ended, or after the next one begun meanwhile")
    {
        std::optional<phazite::Gesture> next;
        phazite::Gesture gesture (*song, "Drag");
        phazite::Gesture::whenEnded (*song, &owner,
                                     [&]
                                     {
                                         ran.push_back ("first");
                                         phazite::Gesture::whenEnded (*song, &owner, note ("at once"));
                                         next.emplace (*song, "Next");
                                         phazite::Gesture::whenEnded (*song, &owner, note ("after the next"));
                                     });
        gesture.commit();
        CHECK (ran == std::vector<juce::String> { "first", "at once" });
        next->commit();
        CHECK (ran == std::vector<juce::String> { "first", "at once", "after the next" });
    }
}

TEST_CASE ("A drag of note edits rolled back, or a trial put back, with steps waiting to be redone: the song as it was found, the same "
           "steps to redo, and Redo doing what it did")
{
    // Undo.h: a Gesture's rollBack() "undoes its changes instead and leaves nothing to redo" of its own, and a Trial's putBack()
    // "takes it all back, leaving nothing to redo"; each keeps the song's own redo steps as they were as it began
    // (redoStepsBefore). Steps begun during either join it. A clip from beat 0 to 4 with two notes.
    using phazite::test::at;
    using phazite::test::lasting;
    phazite::test::TestSong song;
    auto& target = *phazite::findSong (song.context);
    const auto clip = song.addMidiClip (song.firstTrack, 0.0, 4.0);
    const auto first = phazite::addNote (target, clip, 60, at (0.0), lasting (1.0), 100);
    const auto second = phazite::addNote (target, clip, 64, at (1.0), lasting (1.0), 90);
    REQUIRE (first.isValid());
    REQUIRE (second.isValid());
    target.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck check (song.context);

    // The clip and its notes, as the model gives them.
    const auto describe = [&]
    {
        juce::StringArray parts;
        const auto range = phazite::getClipRange (target, clip);
        parts.add ("clip " + juce::String (range.getStart().inBeats()) + "-" + juce::String (range.getEnd().inBeats()));

        for (const auto& note : phazite::getClipNotes (target, clip))
            parts.add (juce::String (note.pitch) + "@" + juce::String (note.start.inBeats()) + " for "
                       + juce::String (note.length.inBeats()) + " v" + juce::String (note.velocity));

        return parts.joinIntoString (", ").toStdString();
    };

    const auto edit = [&] (const juce::String& name, const std::function<void()>& change)
    {
        check.check (name,
                     [&]
                     {
                         const phazite::UndoStep step (target, name);
                         change();
                     });
    };

    // Two note moves, both undone: two steps to redo.
    const auto before = describe();
    edit ("Move First Note", [&] { phazite::moveNote (target, first, at (0.5), 62); });
    edit ("Move Second Note", [&] { phazite::moveNote (target, second, at (2.0), 65); });
    const auto afterBoth = describe();
    check.stepBackOrForth ("Undo", [&target] { phazite::undo (target); });
    check.stepBackOrForth ("Undo", [&target] { phazite::undo (target); });
    const auto found = describe();
    CHECK (found == before);
    const auto steps = phazite::getHistorySteps (target);
    REQUIRE (steps.ahead == juce::StringArray { "Move First Note", "Move Second Note" });

    // What each drag or trial below changes: notes moved, nudged and made louder (a model function that begins a step of its
    // own, which joins it), and the clip moved.
    const auto drag = [&]
    {
        phazite::moveNote (target, first, at (3.0), 72);
        phazite::nudgeNotes (target, { second }, lasting (0.5));
        phazite::changeNoteVelocities (target, { first, second }, 10);
        phazite::moveClip (target, clip, at (8.0), song.firstTrack);
    };

    // Redo then does what it did before: both moves.
    const auto redoBoth = [&]
    {
        CHECK (phazite::canRedo (target));
        check.stepBackOrForth ("Redo", [&target] { phazite::redo (target); });
        check.stepBackOrForth ("Redo", [&target] { phazite::redo (target); });
        CHECK (describe() == afterBoth);
        CHECK_FALSE (phazite::canRedo (target));
    };

    SUBCASE ("A gesture rolled back")
    {
        {
            phazite::Gesture gesture (target, "Drag Notes");
            drag();
            CHECK (describe() != found);
            gesture.rollBack();
        }

        CHECK (describe() == found);
        CHECK (phazite::getHistorySteps (target).ahead == steps.ahead);
        CHECK (phazite::getHistorySteps (target).done == steps.done);
        redoBoth();
    }

    SUBCASE ("A gesture committed: one step, with the step begun during it, and the steps that waited gone")
    {
        check.check ("Drag Notes",
                     [&]
                     {
                         phazite::Gesture gesture (target, "Drag Notes");
                         drag();
                         gesture.commit();
                     });

        const auto dragged = describe();
        CHECK (phazite::getHistorySteps (target).done == juce::StringArray { "Drag Notes" });
        CHECK (phazite::getHistorySteps (target).ahead.isEmpty());
        check.stepBackOrForth ("Undo Drag Notes", [&target] { phazite::undo (target); });
        CHECK (describe() == found);
        check.stepBackOrForth ("Redo Drag Notes", [&target] { phazite::redo (target); });
        CHECK (describe() == dragged);
    }

    SUBCASE ("A trial put back, after one tried from its start again")
    {
        {
            phazite::Trial trial (target, "Swap", nullptr);
            trial.apply (drag);
            CHECK (describe() != found);

            // From its start: what was tried before is taken back first, so only the second note moves.
            trial.apply ([&] { phazite::moveNote (target, second, at (3.0), 67); }, true);
            const auto notes = phazite::getClipNotes (target, clip);
            REQUIRE (notes.size() == 2);
            CHECK (phazite::getClipRange (target, clip).getStart() == at (0.0));
            CHECK (phazite::getNoteData (target, first).value().pitch == 60);
            CHECK (phazite::getNoteData (target, first).value().velocity == 100);
            CHECK (phazite::getNoteData (target, second).value().pitch == 67);
            CHECK (phazite::getNoteData (target, second).value().start == at (3.0));

            trial.putBack();
        }

        CHECK (describe() == found);
        CHECK (phazite::getHistorySteps (target).ahead == steps.ahead);
        CHECK (phazite::getHistorySteps (target).done == steps.done);
        redoBoth();
    }

    check.walkBackAndForth();
}