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
#include "model/commands/CommandRegistry.h"
#include "model/project/ProjectContext.h"

#include <functional>
#include <vector>

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

// A song, or a part of it, as a project file's text.
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

private:
    Song& song;
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
} // namespace phazite::test
