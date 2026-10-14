// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Undo.h"

namespace phazite
{
namespace
{
    // AutomatableParameter::valueTreePropertyChanged() updates a parameter's CachedValue when undo
    // changes it, but not its currentValue, which the audio thread reads. Automated parameters
    // follow their curves instead, so they are left alone.
    void playStoredParameterValues (te::Edit& edit)
    {
        for (auto* parameter : edit.getAllAutomatableParams (true))
            if (parameter->getCurve().getNumPoints() == 0)
                parameter->updateFromAttachedValue();
    }

    // "Undo Change Volume": the verb with the name of the step, when there is a step and it is named.
    juce::String withStep (const juce::String& verb, bool hasStep, const juce::String& step)
    {
        return hasStep && step.isNotEmpty() ? verb + " " + step : verb;
    }
} // namespace

void undo (te::Edit& edit)
{
    edit.undo();
    playStoredParameterValues (edit);
}

void redo (te::Edit& edit)
{
    edit.redo();
    playStoredParameterValues (edit);
}

bool canUndo (te::Edit& edit)
{
    return edit.getUndoManager().canUndo();
}

bool canRedo (te::Edit& edit)
{
    return edit.getUndoManager().canRedo();
}

void undoOrRedo (te::Edit& edit)
{
    if (canRedo (edit))
        redo (edit);
    else
        undo (edit);
}

juce::String describeUndo (te::Edit* edit)
{
    if (edit == nullptr)
        return "Undo";

    auto& undoManager = edit->getUndoManager();
    return withStep ("Undo", undoManager.canUndo(), undoManager.getUndoDescription());
}

juce::String describeRedo (te::Edit* edit)
{
    if (edit == nullptr)
        return "Redo";

    auto& undoManager = edit->getUndoManager();
    return withStep ("Redo", undoManager.canRedo(), undoManager.getRedoDescription());
}

juce::String describeUndoOrRedo (te::Edit* edit)
{
    return edit != nullptr && canRedo (*edit) ? describeRedo (edit) : describeUndo (edit);
}

//==============================================================================
UndoStep::UndoStep (te::Edit& edit, const juce::String& name)
    : undoManager (edit.getUndoManager()),
      outer (current)
{
    auto joinsOuterStep = false;

    for (auto* step = outer; step != nullptr && ! joinsOuterStep; step = step->outer)
        joinsOuterStep = &step->undoManager == &undoManager;

    if (! joinsOuterStep)
        undoManager.beginNewTransaction (name);

    current = this;
}

UndoStep::~UndoStep()
{
    jassert (current == this); // steps end in the reverse order of their beginning, on the message thread
    current = outer;
}
} // namespace phazite
