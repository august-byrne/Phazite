// SPDX-License-Identifier: AGPL-3.0-or-later
// Undo and redo, and the undo convention: every change to a project is one named undo step in the song's history
// (Song::getUndoManager(), model/base/Song.h), which a command (through its undoTarget) or a model function that
// is a change of its own begins with an UndoStep.
#pragma once

#include "model/base/Song.h"
#include "model/base/internal/Selection.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace phazite
{
// Undo and redo for a song: a step back or forth in its history, which what plays follows, the values set by hand
// included. Once the step is back (or done again), what follows the history's moves is told (Song.h: HistoryListener), in
// this call: as each step taken back alone (a gesture rolled back, a trial put back) and each of a version's held steps
// done again or undone is (Trial below).
void undo (Song&);
void redo (Song&);

// How many times a song's history has moved by hand, its own alone (another song's, an export's of an old version, is
// none of it): a step begun (an UndoStep that joins none, a gesture, a trial), an undo or a redo; not the steps recording
// begins as the song plays (RecordingStep). An answer that comes later than what it answers compares it with the count it
// was asked at (a value typed into a plug-in's control: ParameterHandle). Message thread.
std::uint64_t countHistoryMoves (const Song&);

// Whether the song has a change to undo, or an undone change to redo: neither while a trial holds its history
// (Trial::holdHistory(): a version open), whose undo() and redo() do nothing. A redo step is only one of the song's: JUCE's
// history can hold more after a step taken back alone, which Phazite counts (Song::History::staleRedoSteps; JUCE workaround W56
// (docs/juce-workarounds.md)), and nothing here reaches them.
bool canUndo (const Song&);
bool canRedo (const Song&);

// The song's steps as Undo and Redo reach them (Undo History's rows: model/commands/UndoHistory.h): those Undo would take
// back, oldest first, and those Redo would do again, in order. While a trial holds the history (a version open), the song's
// own as they come back at Back to Now, without the step that shows the other.
struct HistorySteps
{
    juce::StringArray done, ahead;
};

HistorySteps getHistorySteps (const Song&);

// The song's history from its last steps on (so many), the steps under them gone (none of them reachable any more), or from
// nothing with none: what a branch made from a version open starts with (model/versions/VersionsModel.cpp). The last of them
// must be a step with changes, just ended. Message thread.
void startHistoryAgain (Song&, int lastSteps);

// FL Studio's undo-or-redo: redoes the change last undone if there is one, else undoes the last change.
void undoOrRedo (Song&);

// The names menus show for undo and redo: "Undo Change Volume" when the song can undo a named step, else "Undo" (also
// without a song); likewise "Redo ...". describeUndoOrRedo() is describeRedo() when the song can redo, else
// describeUndo().
juce::String describeUndo (Song*);
juce::String describeRedo (Song*);
juce::String describeUndoOrRedo (Song*);

//==============================================================================
// Begins a named undo step: it holds every change made through the song's history until the next step begins. Make one
// on the stack, first thing, in a model function that is a change of its own. Made while another one for the same song is
// in progress, it begins nothing: the outer step holds everything. Message thread only; a drag across events is a
// Gesture.
class UndoStep
{
public:
    UndoStep (Song&, const juce::String& name);
    UndoStep (juce::UndoManager&, const juce::String& name); // a song's history, as a command's undo target names it
    ~UndoStep();

    // What a change made that the name its step was given before it ran doesn't say: the step is named for what was made
    // ("Duplicate 2 Notes" where it was "Duplicate 6 Notes"), and what the user should know of it ("4 copies didn't fit")
    // is given out as the step ends, for the hint bar (CommandRegistry.h, takeCommandOutcome()). Only a step begun under
    // the name asked is renamed: a command's own, not a macro's, a drag's or another step's that the change joins.
    // Nothing outside a step for the song.
    static void tellOutcome (Song&, const juce::String& asked, const juce::String& made, const juce::String& outcome);

private:
    juce::UndoManager& undoManager;
    UndoStep* const outer;
    const juce::String name;
    bool begun = false;                        // whether it began the history's transaction, rather than join one
    juce::String outcome;                      // what tellOutcome() said, given out as it ends
    static inline UndoStep* current = nullptr; // the innermost step in progress

    // The outermost of the steps in progress from this one out that is for the history, or none.
    static UndoStep* findOutermostStep (juce::UndoManager&, UndoStep* from);

    JUCE_DECLARE_NON_COPYABLE (UndoStep)
};

//==============================================================================
// The undo step of a drag, or of any change that spans several events (roadmap phase A2): begun when it starts, it
// holds every change made through the song's history as one named step until commit(), and steps begun meanwhile join it.
// rollBack() undoes its changes instead and leaves nothing to redo, as Escape during a drag should. Destroyed without
// either, it commits. Message thread only, one at a time for a song.
class Gesture
{
public:
    Gesture (Song&, const juce::String& name);
    ~Gesture();

    void commit();
    void rollBack();
    bool isOpen() const noexcept { return open; }

    // Whether a gesture is in progress for a song, which a change that starts meanwhile can join.
    static bool isInProgress (Song&);

    // Runs a change once the song's gesture in progress has ended, committed or rolled back, after it, so that it is a step
    // of its own which the gesture's Escape doesn't take back (what recording keeps as playback stops during a drag); at
    // once if none is in progress. The owner given forgets every change it left waiting (forget()), as it goes. A change
    // that begins another gesture leaves those after it waiting for that one.
    static void whenEnded (Song&, const void* owner, std::function<void()> change);
    static void forget (const void* owner);

private:
    Song& song;
    bool open = false;
    int redoStepsBefore = 0; // the song's own redo steps as it began, which a roll back leaves as they were

    JUCE_DECLARE_NON_COPYABLE (Gesture)
};

//==============================================================================
// The undo step of something tried in place until it is kept or put back, as Swap tries sounds and presets (docs/design.md,
// section 11; model/devices/SwapModel.h). What its owner changes through apply() is one step, begun at the first change:
// keep() keeps it, under a name of its own; putBack() takes it all back, leaving nothing to redo. Unlike a Gesture it gives
// way: before a step, a gesture or a recording begins meanwhile, or an undo or a redo, what was tried is kept, by its
// owner's keepFirst (which keeps it or puts it back as it sees fit), so that the next change is a step of its own.
// Destroyed while open, it puts back what was tried. Message thread only, one at a time for a song.
class Trial
{
public:
    Trial (Song&, const juce::String& name, std::function<void()> keepFirst);
    ~Trial();

    // Makes a change in its step, steps begun meanwhile joining it; fromStart takes back what was tried before, first.
    void apply (const std::function<void()>& change, bool fromStart = false);

    // The name of its step, as Undo's menu shows it meanwhile and as it is kept if something else keeps it.
    void rename (const juce::String&);

    bool hasChanges() const;
    void keep (const juce::String& name);
    void putBack();
    bool isOpen() const noexcept { return open; }

    // Keeps the trial open for a song, if it has changes: what a change that begins elsewhere does first. A trial that holds
    // the history (below) isn't kept so: what begins goes on top of it.
    static void keepOpenTrial (juce::UndoManager&);

    // A trial that shows another song for a while rather than one to keep (a version opened live: model/versions/
    // VersionsModel.h, openVersion()) holds the song's history: from now until it closes the song says so
    // (Song::setShowingOther(), which keeps two undo steps more meanwhile, its own and one on top of it, and more as more begin
    // on top of it, so that none takes any of the song's away), its undo and redo do nothing (canUndo() and canRedo() are false), and a write of the song (a
    // save, an export: putBackShowing()) puts it back first by its owner's putBack, so that no file holds what it shows.
    //
    // Before its first change. The song's redo steps are done again first, out of sight, so that what it shows goes on top of
    // them and putBackHeld() takes all of it back with undos alone, and the song's redo steps are what they were (JUCE can't
    // take a step back alone once another began on top of it: JUCE workaround W56 (docs/juce-workarounds.md)). Every change
    // made through apply() starts again from the song's own (fromStart, always).
    //
    // A step that begins meanwhile (an UndoStep, a gesture, a RecordingStep, another trial such as Swap's) goes on top of it
    // as a step of its own, and steps begun inside it go on top too. As the last of them ends, with changes on top of it (an
    // edit of what it shows), its owner's edited (false) takes the edit: it ends the hold, with those steps the newest, or
    // holds on until it can end it (a branch waiting for what it needs: VersionsModel.cpp), steps that end meanwhile joining
    // the edit; refused, the trial is kept as it is. Without changes (a press that changed nothing, a drag ended with Escape,
    // a trial put back), nothing happened, and it holds on. An edit that can't wait for a step to end (recording) asks
    // first: makeWayForEdit(). The history keeps as many more steps meanwhile as can be on top of it (Song::History), so that
    // none of the song's goes as they come.
    struct Holder
    {
        std::function<void()> putBack;                // takes it back: Back to Now, by putBackHeld(); may refuse (open still)
        std::function<bool (bool mustBranch)> edited; // takes the edit (above): whether it did
    };

    void holdHistory (Holder);
    bool holdsHistory() const noexcept { return holding; }

    // While it holds the history: the song's steps on top of its own (the edit's: one, or more for steps begun inside one
    // another), and the undos that take the song back to its own from here (those, its own step if it has changes, and the
    // song's redo steps done again under it).
    int countStepsOnTop() const;
    int countUndosBack() const { return countStepsOnTop() + heldSets + heldRedo; }

    // What a write of a song (a save, an autosave, an export: model/project/internal/Project.h) does first: the trial that
    // holds its history, if one is open, put back by its owner, which may refuse (then nothing is written); one without an
    // owner's way put back as it is. Whether none holds it now.
    static bool putBackShowing (juce::UndoManager&);

    // The trial that holds the history put back: a trial with changes on top of it (Swap's) put back first, by its own
    // putBack(); steps begun on top of it and still open taken back (they go on, as a step of the song's own); its step and
    // the redo steps done again under it undone, so that the song and its redo steps are as they were; then whileHeld,
    // before the song is its own again (Song::setShowingOther (false)), so that what it changes there is no change of the
    // song's either (what isn't in the history, made as it was: VersionsModel.cpp).
    void putBackHeld (const std::function<void()>& whileHeld = {});

    // An edit that changes the song without a step that ends (recording, which starts a branch from a version open before
    // anything is recorded): the trial that holds the song's history, if one does, asked to end the hold by its owner's
    // edited (true). Whether it ended it as asked (nothing holds the history now): false where the owner refused, even if
    // the hold ended (the version kept as a step: its notice says so), and then nothing may record.
    static bool makeWayForEdit (Song&);

    // Whether a step begun on top of the trial that holds a song's history is still open (a drag): nothing that would take
    // the trial back or apply anew meanwhile.
    static bool isStepOpenOnHeld (const Song&);

    // Whether a trial with changes (Swap's) is open on top of the trial that holds a song's history: what it tries is
    // finished first (kept, an edit; or put back) before Back to Now, which would put it back.
    static bool isTrialOnHeld (const Song&);

    // Whether a song has a trial open, changes and all, or one that holds its history: Auto Save waits until it ends
    // (model/project/internal/Project.h).
    static bool isInProgress (const Song&);

    // Whether a song has a trial with changes open, other than one that holds its history: work that changes the song as it
    // ends waits for it (AudioWork.h), while a trial that holds the history takes such work on top of it, as an edit.
    static bool isTrying (const Song&);

    // Its keep under way, waiting for work in the background (Swap's, whose sounds are copied in first: AudioWork.h): it
    // isn't kept a second time meanwhile by a change that begins elsewhere (keepOpenTrial() leaves it be), and no other trial
    // opens for the song until it is kept (isKeepPending(): Swap opens closed).
    void setKeepPending (bool pending) noexcept { keepPending = pending; }
    static bool isKeepPending (const Song&);

private:
    juce::WeakReference<Song> song;
    juce::String name;
    std::function<void()> keepFirst;
    Holder holder;
    bool open = true, begun = false, applying = false, keepPending = false, holding = false;
    int redoStepsBefore = 0; // the song's own redo steps as its step began, which putBack() leaves as they were

    // While it holds the history: its steps (one, or none before a change), the song's redo steps done again under them, and
    // the song's undo steps under what begins on top of it (its own and those included); the steps begun on top of it and
    // not ended yet, by what began them, with the first's name; and whether it is being put back (no edit then).
    int heldSets = 0, heldRedo = 0, heldAt = 0;
    std::vector<const void*> stepsOnTop;
    juce::String stepOnTopName;
    bool puttingBack = false;

    juce::UndoManager* getUndoManager() const;
    void close();
    void undoHeld();
    static void keepOpenTrial (juce::UndoManager&, const Trial* except);
    static Trial* findHolding (const juce::UndoManager&);

    // A step begun or ended by what began it (an UndoStep, a gesture, a trial): told to the trial that holds the history.
    static void stepBegins (juce::UndoManager&, const void* by, const juce::String& stepName);
    static void stepEnds (juce::UndoManager&, const void* by);

    friend class UndoStep;
    friend class Gesture;
    friend class RecordingStep;
    friend HistorySteps getHistorySteps (const Song&);

    JUCE_DECLARE_NON_COPYABLE (Trial)
};

//==============================================================================
// The undo step of what is recorded while the song plays (automation, roadmap step 2.1), which the song takes in as
// recording ends: it begins a named step. Unlike a Gesture, changes made meanwhile begin steps of their own as usual,
// and what is recorded after one of them joins its step. Begun during a gesture, it joins the gesture's step. Made on the
// stack: on a song shown in place (a version open: Trial::holdHistory()), what it records is an edit as any other, told as
// it goes. Message thread only.
class RecordingStep
{
public:
    RecordingStep (Song&, const juce::String& name);
    ~RecordingStep();

private:
    juce::UndoManager& undoManager;
    bool begun = false;

    JUCE_DECLARE_NON_COPYABLE (RecordingStep)
};

//==============================================================================
// For registering commands (CommandRegistry.h) that act on a selection's song, the open project's: the action runs, or the
// test answers, with that song (without one, the action does nothing and the test is false); and its history as the undo
// target of a command that changes it.
std::function<void()> withSong (const Selection&, std::function<void (Song&)> action);
std::function<bool()> ifSong (const Selection&, std::function<bool (const Song&)> test);
std::function<juce::UndoManager*()> undoTargetOf (const Selection&);
} // namespace phazite
