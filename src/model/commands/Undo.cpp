// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Undo.h"
#include "model/base/internal/Schema.h"

#include <algorithm>
#include <vector>

namespace phazite
{
namespace
{
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

// The song's history takes a step back or forth: what plays follows the song (its playback session listens to it), values set
// by hand included.
void undo (Song& song)
{
    song.getUndoManager().undo();
}

void redo (Song& song)
{
    song.getUndoManager().redo();
}

bool canUndo (const Song& song)
{
    return song.getUndoManager().canUndo();
}

bool canRedo (const Song& song)
{
    return song.getUndoManager().canRedo();
}

void undoOrRedo (Song& song)
{
    if (canRedo (song))
        redo (song);
    else
        undo (song);
}

juce::String describeUndo (Song* song)
{
    if (song == nullptr)
        return "Undo";

    auto& undoManager = song->getUndoManager();
    return withStep ("Undo", undoManager.canUndo(), undoManager.getUndoDescription());
}

juce::String describeRedo (Song* song)
{
    if (song == nullptr)
        return "Redo";

    auto& undoManager = song->getUndoManager();
    return withStep ("Redo", undoManager.canRedo(), undoManager.getRedoDescription());
}

juce::String describeUndoOrRedo (Song* song)
{
    return song != nullptr && canRedo (*song) ? describeRedo (song) : describeUndo (song);
}

UndoStep::UndoStep (Song& song, const juce::String& name)
    : UndoStep (song.getUndoManager(), name)
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
Gesture::Gesture (Song& s, const juce::String& name)
    : song (s),
      open (true)
{
    auto& undoManager = song.getUndoManager();
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
    gestures.erase (std::remove (gestures.begin(), gestures.end(), &song.getUndoManager()), gestures.end());
    open = false;
}

bool Gesture::isInProgress (Song& song)
{
    return isInGesture (song.getUndoManager());
}

void Gesture::rollBack()
{
    if (! open)
        return;

    song.getUndoManager().undoCurrentTransactionOnly();
    commit();
}

//==============================================================================
RecordingStep::RecordingStep (Song& song, const juce::String& name)
{
    if (auto& undoManager = song.getUndoManager(); ! isInGesture (undoManager))
        undoManager.beginNewTransaction (name);
}

std::function<void()> withSong (const Selection& selection, std::function<void (Song&)> action)
{
    return [&selection, action]
    {
        if (auto* song = selection.getSong())
            action (*song);
    };
}

std::function<bool()> ifSong (const Selection& selection, std::function<bool (const Song&)> test)
{
    return [&selection, test]
    {
        auto* song = selection.getSong();
        return song != nullptr && test (*song);
    };
}

std::function<juce::UndoManager*()> undoTargetOf (const Selection& selection)
{
    return [&selection]() -> juce::UndoManager*
    {
        auto* song = selection.getSong();
        return song != nullptr ? &song->getUndoManager() : nullptr;
    };
}
} // namespace phazite
