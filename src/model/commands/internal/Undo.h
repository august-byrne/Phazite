// SPDX-License-Identifier: AGPL-3.0-or-later
// Undo and redo, and the undo convention: every change to a project is one named undo step in the song's history
// (Song::getUndoManager(), model/base/Song.h), which a command (through its undoTarget) or a model function that
// is a change of its own begins with an UndoStep.
#pragma once

#include "model/base/Song.h"
#include "model/base/internal/Selection.h"

#include <functional>

namespace phazite
{
// Undo and redo for a song: a step back or forth in its history, which what plays follows, the values set by hand
// included.
void undo (Song&);
void redo (Song&);

// Whether the song has a change to undo, or an undone change to redo.
bool canUndo (const Song&);
bool canRedo (const Song&);

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

private:
    juce::UndoManager& undoManager;
    UndoStep* const outer;
    static inline UndoStep* current = nullptr; // the innermost step in progress

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

private:
    Song& song;
    bool open = false;

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

    // Keeps the trial open for a song, if it has changes: what a change that begins elsewhere does first.
    static void keepOpenTrial (juce::UndoManager&);

    // Whether a song has a trial open, changes and all: Auto Save waits until it ends (model/project/internal/Project.h).
    static bool isInProgress (const Song&);

private:
    juce::WeakReference<Song> song;
    juce::String name;
    std::function<void()> keepFirst;
    bool open = true, begun = false, applying = false;

    juce::UndoManager* getUndoManager() const;
    void close();

    friend class UndoStep;

    JUCE_DECLARE_NON_COPYABLE (Trial)
};

//==============================================================================
// The undo step of what is recorded while the song plays (automation, roadmap step 2.1), which the song takes in as
// recording ends: it begins a named step. Unlike a Gesture, changes made meanwhile begin steps of their own as usual,
// and what is recorded after one of them joins its step. Begun during a gesture, it joins the gesture's step. Message
// thread only.
class RecordingStep
{
public:
    RecordingStep (Song&, const juce::String& name);

private:
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
