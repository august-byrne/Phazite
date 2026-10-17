// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Undo.h"
#include "model/automation/internal/AutomationClips.h"

#include <algorithm>
#include <vector>

namespace phazite
{
namespace
{
    // AutomatableParameter::valueTreePropertyChanged() updates a parameter's CachedValue when undo changes it, but not
    // its currentValue, which the audio thread reads: this tells which stored values an undo or redo changes, so that
    // their parameters play them. Only those: the stored value of one an automation clip sets, which Tracktion writes as
    // the clip plays, isn't its value otherwise. Parameters with a curve of their own follow it instead.
    class StoredValueChanges : private te::AutomatableParameter::Listener
    {
    public:
        explicit StoredValueChanges (te::Edit& edit)
        {
            for (auto* parameter : edit.getAllAutomatableParams (true))
            {
                if (parameter->getCurve().getNumPoints() == 0)
                {
                    watched.add (parameter);
                    parameter->addListener (this);
                }
            }
        }

        ~StoredValueChanges() override
        {
            for (auto* parameter : watched)
                parameter->removeListener (this);
        }

        void play()
        {
            for (auto* parameter : changed)
                parameter->updateFromAttachedValue();
        }

    private:
        juce::ReferenceCountedArray<te::AutomatableParameter> watched; // kept while an undo deletes some
        juce::Array<te::AutomatableParameter*> changed;

        void curveHasChanged (te::AutomatableParameter&) override {}
        void currentValueChanged (te::AutomatableParameter& parameter) override { changed.addIfNotAlreadyThere (&parameter); }

        JUCE_DECLARE_NON_COPYABLE (StoredValueChanges)
    };

    // A clip that undo or redo brings back makes its curves when it has some, but Tracktion puts an automation clip's curve
    // back after the clip, and its parameter's link to it before the curve: asking for its curves makes them, and links
    // them to their parameters.
    void connectAutomationClips (te::Edit& edit)
    {
        for (auto* track : te::getClipTracks (edit))
            for (auto* clip : track->getClips())
                if (isAutomationClip (*clip))
                    clip->getAutomationCurveList (false);
    }

    template <typename UndoOrRedo>
    void undoOrRedo (te::Edit& edit, UndoOrRedo&& undoOrRedoIt)
    {
        StoredValueChanges changes (edit);
        undoOrRedoIt();
        connectAutomationClips (edit);
        changes.play();
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
    undoOrRedo (edit, [&edit] { edit.undo(); });
}

void redo (te::Edit& edit)
{
    undoOrRedo (edit, [&edit] { edit.redo(); });
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
    : UndoStep (edit.getUndoManager(), name)
{
}

UndoStep::UndoStep (juce::UndoManager& manager, const juce::String& name)
    : undoManager (manager),
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

    undoOrRedo (edit, [this] { edit.getUndoManager().undoCurrentTransactionOnly(); });
    commit();
}

//==============================================================================
RecordingStep::RecordingStep (te::Edit& edit, const juce::String& name)
    : inhibitor (edit)
{
    if (! isInGesture (edit.getUndoManager()))
        edit.getUndoManager().beginNewTransaction (name);
}

//==============================================================================
std::function<void()> withEdit (const te::SelectionManager& selection, std::function<void (te::Edit&)> action)
{
    return [&selection, action]
    {
        if (auto* edit = selection.getEdit())
            action (*edit);
    };
}

std::function<bool()> ifEdit (const te::SelectionManager& selection, std::function<bool (te::Edit&)> test)
{
    return [&selection, test]
    {
        auto* edit = selection.getEdit();
        return edit != nullptr && test (*edit);
    };
}

std::function<juce::UndoManager*()> undoTargetOf (const te::SelectionManager& selection)
{
    return [&selection]() -> juce::UndoManager*
    {
        auto* edit = selection.getEdit();
        return edit != nullptr ? &edit->getUndoManager() : nullptr;
    };
}
} // namespace phazite
