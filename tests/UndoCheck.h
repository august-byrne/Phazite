// SPDX-License-Identifier: AGPL-3.0-or-later
// Undo and Redo are exact: a check for any change to a song, which compares the song as text (as a project file and a
// version's pieces write it: model/base/internal/SongText.h) before the change, after it, after Undo (the text before) and
// after Redo (the text after); and, once many changes were made, steps back through all of them and forth again, the song
// at each step the one it was there. A test makes one for its song and runs its changes through it:
//
//     phazite::test::TestSong song;
//     phazite::test::UndoCheck undo (song.context);
//     undo.command (song.commands, "track.add");                                   // a command, as a menu runs it
//     undo.check ("Add Send", [&] { phazite::addSendTo (song.context, a, b); });   // a model function that is a change of its own
//     undo.checkSeveral ("a clip with notes", [&] { ... });                        // the test's own changes, of several steps
//     undo.walkBackAndForth();                                                     // every step back to where it began, and forth
//
// A failure says what differs, by its place in the song: "PHAZITE/TRACKS/TRACK#1004/CLIP#1012: missing".
//
// With each comparison it checks that the order of its tracks the song keeps is the one they make afresh
// (TestTracks.h: holdsExactTrackOrder()), and that the song's values hold together (SongCheck.h: findValueProblems()):
// among them, that no area holds anything of a track out of the song but the kept entries of the keyed and outside values
// (what takes a track out erases what it holds first, model/arrangement/internal/Tracks.h: eraseTrackHoldings()).
//
// It also keeps a player of the song and compiles its plan after each change, Undo and Redo, as a render does: while the
// tests' checking is on, what a plan keeps from the plan before is compared with the song (SongPlayer::setChecking()).
//
// What the comparisons leave out, on purpose, is what no undo step keeps (model/base/internal/Schema.h says of each name
// which kind it is):
//   - view settings, how the song plays (the loop, the metronome, where playback starts, the punch, the input chosen last)
//     and the count of IDs, which an undo never takes back: what schema::isUncountedProperty() and isUncountedElement()
//     name, the same that a project's unsaved changes and the versions' differences leave out;
//   - what the project remembers outside the undo history: the key set last (LASTKEY), which tracks are frozen (a change
//     to what a track froze unfreezes it, and its undo doesn't freeze it again), and what a plug-in keeps of its own (its
//     state, program, format ID and version, and its values set by hand), stored as the song saves;
//   - and, only where changes were made between the steps compared (the walk, and a step back or forth), the switches
//     that are no undo steps: a track's mute, solo and automation mode, and a chain's mute and solo. Around one change
//     they are compared: a change that is an undo step leaves them alone, or changes them in its step (as Bounce in Place
//     mutes its track).
// Nothing else: the order of a parent's elements is compared too, as an undo that puts a clip back in another place among
// its track's elements is not exact (the next undo finds elements by their places).
#pragma once

#include "TestHelpers.h"
#include "model/base/Song.h"
#include "model/base/internal/SongState.h"
#include "model/commands/CommandRegistry.h"
#include "model/project/ProjectContext.h"

#include <functional>
#include <memory>
#include <vector>

namespace phazite
{
class Project;
class SongPlayer;
} // namespace phazite

namespace phazite::test
{
// How much of a song its text holds: all of it; what an undo step keeps; or that without the switches that are no undo
// steps (above).
enum class SongPart
{
    whole,
    undoable,
    undoableWithoutSwitches
};

// A song, or a part of it, as a project file's text, without where it stands in its project's versions (the branch and
// the version it grew from, which the history writes whenever a version is saved, a project's first in the background).
juce::String writeSong (const Song&, SongPart);

// What differs between two such texts, a line each, by its place in the song; a few lines at most. Empty for the same text.
juce::StringArray describeSongDifferences (const juce::String& expected, const juce::String& actual);

class UndoCheck
{
public:
    // Follows a song from here on: the open project's, or this one. It lets the song's history keep every step (a song
    // keeps the last 30 at least), so that the walk reaches back to here. Changes the test makes without it meanwhile
    // (more of the song built, something undone) are steps it walks through too, as the history has them at its next check.
    explicit UndoCheck (Song&);
    explicit UndoCheck (ProjectContext&);
    ~UndoCheck();

    // A change as the UI makes one, a command or a model function that is a change of its own: it makes one undo step, or
    // none if it changes nothing that an undo step keeps (a switch, a view setting, the selection). Checks (CHECK) that
    // Undo then gives the song as it was before, and Redo the song as it was after; that a change without a step left
    // what undo steps keep alone; and that it made no more than one. The song is left as the change left it.
    void check (const juce::String& what, const std::function<void()>& change);

    // A command run as a menu or a key runs it, which must be there and enabled (REQUIRE): a change as above.
    void command (CommandRegistry&, const juce::String& id);

    // The test's own changes, made with any of the model's functions: begun as a step of their own (so that functions
    // which join their caller's step don't join the step before), they may make several. Undoing them all gives the song
    // as it was before, and redoing them all the song as it was after.
    void checkSeveral (const juce::String& what, const std::function<void()>& changes);

    // An undo or a redo that the test runs itself (Edit > Undo, a click in the undo history): the song is then as it was
    // at that step.
    void stepBackOrForth (const juce::String& what, const std::function<void()>& undoOrRedo);

    // Undoes every step made since the check began, the song at each as it was there, and redoes them all, the same;
    // then the whole song, view settings and all, is as it was before the walk. What changes outside the undo history
    // (a lane shown or removed, a track muted) must leave every step's undo and redo finding what it changed.
    void walkBackAndForth();

    // How many undo steps the changes checked have made.
    int getNumSteps() const noexcept { return numSteps; }

    // What a failure is told to, in place of failing the test (FAIL_CHECK): for the tests of the check itself.
    std::function<void (const juce::String& failure)> onFailure;

    // Told at each moment the check leaves the song at (after a change, after its Undo, after its Redo, after an undo or a
    // redo the test runs), with which: what a test checks at every one of them (SavedCopyCheck, below).
    std::function<void (const juce::String& when)> atEachMoment;

private:
    Song& song;

    // A player of the song (model/playback/internal/SongPlayer.h), which compiles a plan after each change, Undo and Redo,
    // so that, while checking is on, what each plan keeps from the plan before is compared with the song: a mark of a
    // change missed shows here, where the edits are.
    std::unique_ptr<SongPlayer> player;
    void compilePlan();
    int base = 0;                      // the history's steps as the check began
    std::vector<juce::String> history; // the song (undoableWithoutSwitches) at each step since, the first as the check began
    int numSteps = 0;

    int getDepth() const; // the steps the history can undo
    void catchUp();
    void run (const juce::String& what, const std::function<void()>& change, bool severalSteps);
    void compare (const juce::String& what, const juce::String& when, const juce::String& expected, const juce::String& actual);
    void fail (const juce::String& failure);

    JUCE_DECLARE_NON_COPYABLE (UndoCheck)
};
// Whether a project's three answers to "is the song as saved?" agree (CHECK): its quick check (Project::hasUnsavedChanges(),
// area by area, from what was marked since the saved copy was taken), its whole comparison with its saved copy
// (Project::isWholeAsSaved(): model/base/internal/WholeState.h, holdSameSong()), and the two files' texts, the saved state's and
// the song's now as a save writes them (writeSongTree(), writeProjectText()), with what counts alone, as the old whole
// comparison read them (Schema.h: isUncountedProperty(), isUncountedElement(); the root's places in no order). Each check then
// takes the song as it is as the saved copy (Project::takeAsSavedForTests()), so that the next compares what changed since.
// The quick check is left out while a version is shown in the song's place (its mark held then: Song::isShowingOther()), and
// where a plug-in changed itself outside the history since (which it counts as a change on its own).
class SavedCopyCheck
{
public:
    explicit SavedCopyCheck (Project&);
    void check (const juce::String& when);

    // How many moments were checked, and how many of them found the song unsaved.
    int getNumChecked() const noexcept { return checked; }
    int getNumUnsaved() const noexcept { return unsaved; }

private:
    Project& project;
    SongState saved {};
    int outsideAtSave = 0;
    int checked = 0, unsaved = 0;
    void takeAsSaved();
};
} // namespace phazite::test
