// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Undo.h"
#include "model/base/internal/Schema.h"
#include "model/mirror/internal/Mirror.h"

#include <algorithm>
#include <vector>

namespace phazite
{
namespace
{
    // The song's history takes a step back or forth, and the mirror brings Tracktion's Edit in line with it
    // (docs/tracktion-migration.md, T1 step 11): what plays follows the song, values set by hand included.
    template <typename UndoOrRedo>
    void undoOrRedo (te::Edit& edit, UndoOrRedo&& undoOrRedoIt)
    {
        undoOrRedoIt();
        mirror::reconcileAll (edit);
    }

    // The song's histories with a gesture in progress (message thread only).
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
    undoOrRedo (edit, [&edit] { getSongUndoManager (edit).undo(); });
}

void redo (te::Edit& edit)
{
    undoOrRedo (edit, [&edit] { getSongUndoManager (edit).redo(); });
}

bool canUndo (te::Edit& edit)
{
    return getSongUndoManager (edit).canUndo();
}

bool canRedo (te::Edit& edit)
{
    return getSongUndoManager (edit).canRedo();
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

    auto& undoManager = getSongUndoManager (*edit);
    return withStep ("Undo", undoManager.canUndo(), undoManager.getUndoDescription());
}

juce::String describeRedo (te::Edit* edit)
{
    if (edit == nullptr)
        return "Redo";

    auto& undoManager = getSongUndoManager (*edit);
    return withStep ("Redo", undoManager.canRedo(), undoManager.getRedoDescription());
}

juce::String describeUndoOrRedo (te::Edit* edit)
{
    return edit != nullptr && canRedo (*edit) ? describeRedo (edit) : describeUndo (edit);
}

//==============================================================================
UndoStep::UndoStep (te::Edit& edit, const juce::String& name)
    : UndoStep (getSongUndoManager (edit), name)
{
}

UndoStep::UndoStep (juce::UndoManager& manager, const juce::String& name)
    : undoManager (manager),
      outer (current)
{
    // Within a gesture, or another step for the same song, it joins that one.
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
      open (true)
{
    auto& undoManager = getSongUndoManager (edit);
    jassert (! isInGesture (undoManager)); // one at a time for a song
    undoManager.beginNewTransaction (name);
    getOpenGestures().push_back (&undoManager);
}

Gesture::~Gesture()
{
    commit();
}

void Gesture::commit()
{
    if (! open)
        return;

    auto& gestures = getOpenGestures();
    gestures.erase (std::remove (gestures.begin(), gestures.end(), &getSongUndoManager (edit)), gestures.end());
    open = false;
}

bool Gesture::isInProgress (te::Edit& edit)
{
    return isInGesture (getSongUndoManager (edit));
}

void Gesture::rollBack()
{
    if (! open)
        return;

    undoOrRedo (edit, [this] { getSongUndoManager (edit).undoCurrentTransactionOnly(); });
    commit();
}

//==============================================================================
RecordingStep::RecordingStep (te::Edit& edit, const juce::String& name)
{
    if (auto& undoManager = getSongUndoManager (edit); ! isInGesture (undoManager))
        undoManager.beginNewTransaction (name);
}

//==============================================================================
std::function<void()> withEdit (const Selection& selection, std::function<void (te::Edit&)> action)
{
    return [&selection, action]
    {
        if (auto* edit = selection.getEdit())
            action (*edit);
    };
}

std::function<bool()> ifEdit (const Selection& selection, std::function<bool (te::Edit&)> test)
{
    return [&selection, test]
    {
        auto* edit = selection.getEdit();
        return edit != nullptr && test (*edit);
    };
}

std::function<juce::UndoManager*()> undoTargetOf (const Selection& selection)
{
    return [&selection]() -> juce::UndoManager*
    {
        auto* edit = selection.getEdit();
        return edit != nullptr ? &getSongUndoManager (*edit) : nullptr;
    };
}
} // namespace phazite
