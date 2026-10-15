// SPDX-License-Identifier: AGPL-3.0-or-later
// Undo and redo, and the undo convention: every change to a project is one named undo step, which a command
// (through its undoTarget) or a model function that is a change of its own begins with an UndoStep.
#pragma once

#include "model/base/Tracktion.h"

#include <memory>

namespace phazite
{
// Undo and redo for an Edit. Use these, not Edit::undo() and redo(): Tracktion's undo puts back a
// parameter's stored value (a track's volume, say) but not the value the engine plays with, so
// these then set every parameter that isn't automated to its stored value.
void undo (te::Edit&);
void redo (te::Edit&);

// Whether the Edit has a change to undo, or an undone change to redo.
bool canUndo (te::Edit&);
bool canRedo (te::Edit&);

// FL Studio's undo-or-redo: redoes the change last undone if there is one, else undoes the last change.
void undoOrRedo (te::Edit&);

// The names menus show for undo and redo: "Undo Change Volume" when the Edit can undo a named step, else "Undo" (also
// without an Edit); likewise "Redo ...". describeUndoOrRedo() is describeRedo() when the Edit can redo, else
// describeUndo().
juce::String describeUndo (te::Edit*);
juce::String describeRedo (te::Edit*);
juce::String describeUndoOrRedo (te::Edit*);

//==============================================================================
// Begins a named undo step: it holds every change made through the Edit's undo manager until the next step begins
// (Tracktion also begins an unnamed one about 350 ms after a change, unless a mouse button is down). Make one on the
// stack, first thing, in a model function that is a change of its own. Made while another one for the same Edit is in
// progress, it begins nothing: the outer step holds everything. Message thread only; a drag across events is a
// Gesture.
class UndoStep
{
public:
    UndoStep (te::Edit&, const juce::String& name);
    ~UndoStep();

private:
    juce::UndoManager& undoManager;
    UndoStep* const outer;
    static inline UndoStep* current = nullptr; // the innermost step in progress

    JUCE_DECLARE_NON_COPYABLE (UndoStep)
};

//==============================================================================
// The undo step of a drag, or of any change that spans several events (roadmap phase A2): begun when it starts, it
// holds every change made through the Edit's undo manager as one named step until commit(), and Tracktion doesn't
// split it meanwhile (it otherwise begins a new step about 350 ms after a change). rollBack() undoes its changes
// instead and leaves nothing to redo, as Escape during a drag should. Destroyed without either, it commits. Message
// thread only, one at a time for an Edit.
class Gesture
{
public:
    Gesture (te::Edit&, const juce::String& name);
    ~Gesture();

    void commit();
    void rollBack();
    bool isOpen() const noexcept { return inhibitor != nullptr; }

private:
    te::Edit& edit;
    std::unique_ptr<te::Edit::UndoTransactionInhibitor> inhibitor;

    JUCE_DECLARE_NON_COPYABLE (Gesture)
};
} // namespace phazite
