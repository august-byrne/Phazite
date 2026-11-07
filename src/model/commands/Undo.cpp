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

    // The trials open, each for its song's history (message thread only).
    std::vector<Trial*>& getOpenTrials()
    {
        static std::vector<Trial*> open;
        return open;
    }

    bool isOpenTrial (const Trial* trial)
    {
        const auto& open = getOpenTrials();
        return std::find (open.begin(), open.end(), trial) != open.end();
    }

    // "Undo Change Volume": the verb with the name of the step, when there is a step and it is named.
    juce::String withStep (const juce::String& verb, bool hasStep, const juce::String& step)
    {
        return hasStep && step.isNotEmpty() ? verb + " " + step : verb;
    }
} // namespace

// The song's history takes a step back or forth: what plays follows the song (its playback session listens to it), values set
// by hand included. What a trial tries is kept first, so that an undo takes it back, as a step that can be redone.
void undo (Song& song)
{
    Trial::keepOpenTrial (song.getUndoManager());
    song.getUndoManager().undo();
}

void redo (Song& song)
{
    Trial::keepOpenTrial (song.getUndoManager());
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
    // Within a gesture, a trial's change, or another step for the same song, it joins that one. Else what a trial tried is
    // kept first, as a step of its own.
    auto joinsOuterStep = isInGesture (undoManager);

    for (auto* trial : getOpenTrials())
        joinsOuterStep = joinsOuterStep || (trial->applying && trial->getUndoManager() == &undoManager);

    for (auto* step = outer; step != nullptr && ! joinsOuterStep; step = step->outer)
        joinsOuterStep = &step->undoManager == &undoManager;

    if (! joinsOuterStep)
    {
        Trial::keepOpenTrial (undoManager);
        undoManager.beginNewTransaction (name);
    }

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
    Trial::keepOpenTrial (undoManager);
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
Trial::Trial (Song& s, const juce::String& stepName, std::function<void()> keepWhatWasTried)
    : song (&s),
      name (stepName),
      keepFirst (std::move (keepWhatWasTried))
{
    jassert (! isInProgress (s)); // one at a time for a song
    getOpenTrials().push_back (this);
}

Trial::~Trial()
{
    putBack();
}

juce::UndoManager* Trial::getUndoManager() const
{
    return song != nullptr ? &song->getUndoManager() : nullptr;
}

bool Trial::hasChanges() const
{
    // Begun, and with changes since: a step that began elsewhere meanwhile, while it had none, made it begin again.
    auto* undoManager = getUndoManager();
    return open && begun && undoManager != nullptr && undoManager->getNumActionsInCurrentTransaction() > 0;
}

void Trial::apply (const std::function<void()>& change, bool fromStart)
{
    auto* undoManager = getUndoManager();

    if (! open || undoManager == nullptr)
        return;

    if (fromStart && hasChanges())
    {
        undoManager->undoCurrentTransactionOnly();
        begun = false;
    }

    if (! hasChanges())
    {
        undoManager->beginNewTransaction (name);
        begun = true;
    }

    const juce::ScopedValueSetter<bool> applyingNow (applying, true);
    change();
}

void Trial::rename (const juce::String& newName)
{
    name = newName;

    if (hasChanges())
        getUndoManager()->setCurrentTransactionName (name);
}

void Trial::keep (const juce::String& keptName)
{
    if (! open)
        return;

    if (hasChanges())
        getUndoManager()->setCurrentTransactionName (keptName);

    close();
}

void Trial::putBack()
{
    if (! open)
        return;

    if (hasChanges())
        getUndoManager()->undoCurrentTransactionOnly();

    close();
}

void Trial::close()
{
    open = false;
    auto& trials = getOpenTrials();
    trials.erase (std::remove (trials.begin(), trials.end(), this), trials.end());
}

void Trial::keepOpenTrial (juce::UndoManager& undoManager)
{
    const auto trials = getOpenTrials(); // a copy: keeping one closes it

    for (auto* trial : trials)
    {
        if (trial->getUndoManager() != &undoManager || trial->applying)
            continue;

        // Without changes, it begins again at its next one, after the step beginning now.
        if (! trial->hasChanges())
        {
            trial->begun = false;
            continue;
        }

        if (trial->keepFirst != nullptr)
            trial->keepFirst();

        if (isOpenTrial (trial))
            trial->keep (trial->name);
    }
}

bool Trial::isInProgress (const Song& song)
{
    for (auto* trial : getOpenTrials())
        if (trial->getUndoManager() == &song.getUndoManager() && trial->hasChanges())
            return true;

    return false;
}

//==============================================================================
RecordingStep::RecordingStep (Song& song, const juce::String& name)
{
    if (auto& undoManager = song.getUndoManager(); ! isInGesture (undoManager))
    {
        Trial::keepOpenTrial (undoManager);
        undoManager.beginNewTransaction (name);
    }
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
