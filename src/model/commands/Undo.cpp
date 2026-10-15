// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Undo.h"

#include <algorithm>
#include <vector>

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

    // The undo managers of the Edits with a gesture in progress (message thread only).
    std::vector<juce::UndoManager*>& getOpenGestures()
    {
        static std::vector<juce::UndoManager*> open;
        return open;
    }

    bool isInGesture (juce::UndoManager& undoManager)
    {
        const auto& open = getOpenGestures();
        return std::find (open.begin(), open.end(), &undoManager) != open.end();
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
    // Within a gesture, or another step for the same Edit, it joins that one.
    auto joinsOuterStep = isInGesture (undoManager);

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

//==============================================================================
Gesture::Gesture (te::Edit& e, const juce::String& name)
    : edit (e),
      inhibitor (std::make_unique<te::Edit::UndoTransactionInhibitor> (e))
{
    jassert (! isInGesture (edit.getUndoManager())); // one at a time for an Edit
    edit.getUndoManager().beginNewTransaction (name);
    getOpenGestures().push_back (&edit.getUndoManager());
}

Gesture::~Gesture()
{
    commit();
}

void Gesture::commit()
{
    if (inhibitor == nullptr)
        return;

    auto& open = getOpenGestures();
    open.erase (std::remove (open.begin(), open.end(), &edit.getUndoManager()), open.end());
    inhibitor.reset();
}

bool Gesture::isInProgress (te::Edit& edit)
{
    return isInGesture (edit.getUndoManager());
}

void Gesture::rollBack()
{
    if (inhibitor == nullptr)
        return;

    edit.getUndoManager().undoCurrentTransactionOnly();
    playStoredParameterValues (edit);
    commit();
}
} // namespace phazite
