// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Undo.h"
#include "CommandRegistry.h"
#include "model/base/internal/Schema.h"

#include <algorithm>
#include <functional>
#include <map>
#include <utility>
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

    // The changes waiting for a song's gesture to end (Gesture::whenEnded()), each with its history and its owner, in order
    // (message thread only).
    struct WaitingChange
    {
        juce::UndoManager* undoManager = nullptr;
        const void* owner = nullptr;
        std::function<void()> change;
    };

    std::vector<WaitingChange>& getWaitingChanges()
    {
        static std::vector<WaitingChange> waiting;
        return waiting;
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

    // What the outermost step that ended last said of what it made (UndoStep::tellOutcome()), until it is taken or the next
    // begins (message thread only).
    juce::String& getStepOutcome()
    {
        static juce::String outcome;
        return outcome;
    }

    // Each history's moves by hand (countHistoryMoves()), by its undo manager (message thread only).
    std::map<const juce::UndoManager*, std::uint64_t>& getHistoryMoves()
    {
        static std::map<const juce::UndoManager*, std::uint64_t> moves;
        return moves;
    }

    void noteHistoryMove (const juce::UndoManager& undoManager)
    {
        ++getHistoryMoves()[&undoManager];
    }

    // "Undo Change Volume": the verb with the name of the step, when there is a step and it is named.
    juce::String withStep (const juce::String& verb, bool hasStep, const juce::String& step)
    {
        return hasStep && step.isNotEmpty() ? verb + " " + step : verb;
    }

    //==========================================================================
    // The redo steps that are the song's (JUCE workaround W56 (docs/juce-workarounds.md)). JUCE puts the redo steps aside as a
    // step is performed while there are some, empties what it put aside only then, and appends all it holds whenever a step is
    // taken back alone (undoCurrentTransactionOnly(): a drag's Escape, a trial put back); so a step taken back alone where
    // there were no redo steps as it began appended those put aside long before, which are none of the song's. They are
    // always the last of its redo steps: Phazite counts them (Song::History::staleRedoSteps), and nothing reaches past the
    // song's own.
    Song::History* findSongHistory (juce::UndoManager& undoManager)
    {
        return dynamic_cast<Song::History*> (&undoManager);
    }

    const Song::History* findSongHistory (const juce::UndoManager& undoManager)
    {
        return dynamic_cast<const Song::History*> (&undoManager);
    }

    int countJuceRedoSteps (const juce::UndoManager& undoManager)
    {
        return undoManager.getRedoDescriptions().size();
    }

    // The steps an undo can take back, all of them (none counted past: an undo reaches each).
    int countUndoSteps (const juce::UndoManager& undoManager)
    {
        return undoManager.getUndoDescriptions().size();
    }

    int countRedoSteps (const juce::UndoManager& undoManager)
    {
        const auto all = countJuceRedoSteps (undoManager);
        const auto* history = findSongHistory (undoManager);
        return history != nullptr ? juce::jmax (0, all - history->staleRedoSteps) : all;
    }

    // As a step begins, and at an undo or a redo: no more of them than JUCE holds (a step performed since put every redo step
    // aside, or the history was cleared), which a step's beginning can't be in the middle of.
    void settleRedoSteps (juce::UndoManager& undoManager)
    {
        if (auto* history = findSongHistory (undoManager))
            history->staleRedoSteps = juce::jmin (history->staleRedoSteps, countJuceRedoSteps (undoManager));
    }

    // What follows the song's history as it moves (Song.h: HistoryListener), told once a move is whole: after each of JUCE's
    // undo() and redo() and each step taken back alone has returned, never from inside one.
    void tellMoved (juce::UndoManager& undoManager)
    {
        if (auto* history = findSongHistory (undoManager))
            history->tellMoved();
    }

    void undoStep (juce::UndoManager& undoManager)
    {
        undoManager.undo();
        tellMoved (undoManager);
    }

    void redoStep (juce::UndoManager& undoManager)
    {
        undoManager.redo();
        tellMoved (undoManager);
    }

    // A step taken back alone, the song's own redo steps as many as as it began: those JUCE appends past them aren't.
    void takeBackAlone (juce::UndoManager& undoManager, int redoStepsBefore)
    {
        undoManager.undoCurrentTransactionOnly();

        if (auto* history = findSongHistory (undoManager))
            history->staleRedoSteps = juce::jmax (0, countJuceRedoSteps (undoManager) - redoStepsBefore);

        tellMoved (undoManager);
    }

    // What startHistoryAgain() performs into the last step to make JUCE drop every step under it: nothing.
    struct NoChange final : juce::UndoableAction
    {
        bool perform() override { return true; }
        bool undo() override { return true; }
    };
} // namespace

// The song's history takes a step back or forth: what plays follows the song (its playback session listens to it), values set
// by hand included. What a trial tries is kept first, so that an undo takes it back, as a step that can be redone.
std::uint64_t countHistoryMoves (const Song& song)
{
    const auto& moves = getHistoryMoves();
    const auto found = moves.find (&song.getUndoManager());
    return found != moves.end() ? found->second : 0;
}

void undo (Song& song)
{
    if (song.isShowingOther())
        return; // a trial holds the history (Trial::holdHistory())

    auto& undoManager = song.getUndoManager();
    Trial::keepOpenTrial (undoManager);
    settleRedoSteps (undoManager);
    noteHistoryMove (undoManager);
    undoStep (undoManager);
}

void redo (Song& song)
{
    if (song.isShowingOther())
        return;

    auto& undoManager = song.getUndoManager();
    Trial::keepOpenTrial (undoManager);
    settleRedoSteps (undoManager);

    if (countRedoSteps (undoManager) == 0)
        return; // none of the song's

    noteHistoryMove (undoManager);
    redoStep (undoManager);
}

bool canUndo (const Song& song)
{
    return ! song.isShowingOther() && song.getUndoManager().canUndo();
}

bool canRedo (const Song& song)
{
    return ! song.isShowingOther() && countRedoSteps (song.getUndoManager()) > 0;
}

HistorySteps getHistorySteps (const Song& song)
{
    // JUCE lists the steps Undo would take back newest first, and those Redo would do again oldest first.
    const auto& undoManager = song.getUndoManager();
    auto done = undoManager.getUndoDescriptions();
    auto ahead = undoManager.getRedoDescriptions();
    ahead.removeRange (countRedoSteps (undoManager), ahead.size());

    // While a trial holds the history, under the steps on top of it and its own: the song's redo steps, done again.
    if (const auto* held = Trial::findHolding (undoManager))
    {
        const auto above = held->countStepsOnTop() + held->heldSets;
        ahead.clear();

        for (int i = 0; i < held->heldRedo; ++i)
            ahead.insert (0, done[above + i]);

        done.removeRange (0, above + held->heldRedo);
    }

    HistorySteps steps;
    steps.ahead = ahead;

    for (int i = done.size(); --i >= 0;)
        steps.done.add (done[i]);

    return steps;
}

void startHistoryAgain (Song& song, int lastSteps)
{
    auto& undoManager = song.getUndoManager();
    auto* history = findSongHistory (undoManager);

    if (lastSteps <= 0 || undoManager.getNumActionsInCurrentTransaction() == 0)
    {
        jassert (lastSteps <= 0); // the last a step with changes, just ended
        undoManager.clearUndoHistory();
        settleRedoSteps (undoManager);
        return;
    }

    // JUCE drops the oldest steps only as one is performed, while it holds more than it keeps: keeping so many, a change of
    // nothing performed into the last step drops every step under them (JUCE workaround W56 (docs/juce-workarounds.md)).
    undoManager.setMaxNumberOfStoredUnits (1, lastSteps);
    undoManager.perform (new NoChange());

    if (history != nullptr)
        history->keepMoreSteps (song.isShowingOther() ? 2 : 0);
    else
        undoManager.setMaxNumberOfStoredUnits (1, Song::defaultUndoSteps);

    settleRedoSteps (undoManager);
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

    if (song->isShowingOther())
        return "Undo (Back to Now first)"; // greyed: a version is open (Trial::holdHistory())

    auto& undoManager = song->getUndoManager();
    return withStep ("Undo", undoManager.canUndo(), undoManager.getUndoDescription());
}

juce::String describeRedo (Song* song)
{
    if (song == nullptr)
        return "Redo";

    if (song->isShowingOther())
        return "Redo (Back to Now first)";

    return withStep ("Redo", canRedo (*song), song->getUndoManager().getRedoDescription());
}

juce::String describeUndoOrRedo (Song* song)
{
    return song != nullptr && canRedo (*song) ? describeRedo (song) : describeUndo (song);
}

UndoStep::UndoStep (Song& song, const juce::String& stepName)
    : UndoStep (song.getUndoManager(), stepName)
{
}

UndoStep::UndoStep (juce::UndoManager& manager, const juce::String& stepName)
    : undoManager (manager),
      outer (current),
      name (stepName)
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
        settleRedoSteps (undoManager);
        noteHistoryMove (undoManager);
        undoManager.beginNewTransaction (name);
        begun = true;
        Trial::stepBegins (undoManager, this, name);
    }

    if (findOutermostStep (undoManager, outer) == nullptr)
        getStepOutcome().clear(); // what an earlier step said is no news of this one

    current = this;
}

UndoStep::~UndoStep()
{
    jassert (current == this); // steps end in the reverse order of their beginning, on the message thread
    current = outer;

    if (findOutermostStep (undoManager, outer) == nullptr)
        getStepOutcome() = outcome;

    if (begun)
        Trial::stepEnds (undoManager, this);
}

UndoStep* UndoStep::findOutermostStep (juce::UndoManager& manager, UndoStep* from)
{
    UndoStep* found = nullptr;

    for (auto* step = from; step != nullptr; step = step->outer)
        if (&step->undoManager == &manager)
            found = step;

    return found;
}

void UndoStep::tellOutcome (Song& song, const juce::String& asked, const juce::String& made, const juce::String& said)
{
    auto* step = findOutermostStep (song.getUndoManager(), current);

    if (step == nullptr)
        return;

    if (step->begun && step->name == asked && made.isNotEmpty())
        song.getUndoManager().setCurrentTransactionName (made);

    step->outcome = said;
}

juce::String takeCommandOutcome()
{
    return std::exchange (getStepOutcome(), {});
}

//==============================================================================
Gesture::Gesture (Song& s, const juce::String& name)
    : song (s),
      open (true)
{
    auto& undoManager = song.getUndoManager();
    jassert (! isInGesture (undoManager)); // one at a time for a song
    Trial::keepOpenTrial (undoManager);
    settleRedoSteps (undoManager);
    redoStepsBefore = countRedoSteps (undoManager);
    noteHistoryMove (undoManager);
    undoManager.beginNewTransaction (name);
    getOpenGestures().push_back (&undoManager);
    Trial::stepBegins (undoManager, this, name);
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
    auto* undoManager = &song.getUndoManager();
    gestures.erase (std::remove (gestures.begin(), gestures.end(), undoManager), gestures.end());
    open = false;

    // An edit of a song shown in place (a version open) ends what shows it first, so that what waited is a step after it.
    Trial::stepEnds (*undoManager, this);

    // What waited for it, now, in order: each takes it out before it runs, so that one it adds meanwhile waits on.
    for (auto& waiting = getWaitingChanges(); ! isInGesture (*undoManager);) // until a change begins another gesture
    {
        const auto next =
            std::find_if (waiting.begin(), waiting.end(), [undoManager] (const auto& each) { return each.undoManager == undoManager; });

        if (next == waiting.end())
            break;

        const auto change = std::move (next->change);
        waiting.erase (next);
        change();
    }
}

bool Gesture::isInProgress (Song& song)
{
    return isInGesture (song.getUndoManager());
}

void Gesture::whenEnded (Song& song, const void* owner, std::function<void()> change)
{
    if (! isInProgress (song))
    {
        change();
        return;
    }

    getWaitingChanges().push_back ({ &song.getUndoManager(), owner, std::move (change) });
}

void Gesture::forget (const void* owner)
{
    std::erase_if (getWaitingChanges(), [owner] (const auto& each) { return each.owner == owner; });
}

void Gesture::rollBack()
{
    if (! open)
        return;

    takeBackAlone (song.getUndoManager(), redoStepsBefore);
    commit();
}

//==============================================================================
Trial::Trial (Song& s, const juce::String& stepName, std::function<void()> keepWhatWasTried)
    : song (&s),
      name (stepName),
      keepFirst (std::move (keepWhatWasTried))
{
    // One trial with changes at a time for a history: one tried meanwhile is kept first, as a step beginning would keep it.
    keepOpenTrial (s.getUndoManager());
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
    // Holding the history: with its step. Else begun, and with changes since: a step that began elsewhere meanwhile, while it
    // had none, made it begin again.
    auto* undoManager = getUndoManager();

    if (holding)
        return open && heldSets > 0;

    return open && begun && undoManager != nullptr && undoManager->getNumActionsInCurrentTransaction() > 0;
}

void Trial::apply (const std::function<void()>& change, bool fromStart)
{
    auto* undoManager = getUndoManager();

    if (! open || undoManager == nullptr)
        return;

    // Holding the history: from the song's own, always, its step undone first; not under a step begun on top of it, nor
    // with changes on top of it.
    if (holding)
    {
        jassert (stepsOnTop.empty() && countStepsOnTop() == 0);

        if (! stepsOnTop.empty() || countStepsOnTop() > 0)
            return;

        undoHeld();
        noteHistoryMove (*undoManager);
        undoManager->beginNewTransaction (name);
        begun = true;

        {
            const juce::ScopedValueSetter<bool> applyingNow (applying, true);
            change();
        }

        heldSets = undoManager->getNumActionsInCurrentTransaction() > 0 ? 1 : 0;
        heldAt = countUndoSteps (*undoManager);
        return;
    }

    if (fromStart && hasChanges())
    {
        takeBackAlone (*undoManager, redoStepsBefore);
        begun = false;
    }

    if (! hasChanges())
    {
        // Another trial of the history with changes is kept first, closed, so that each has its own step: begun alongside it,
        // each counted the other's changes as its own, and one kept or put back the other's with its own (2026-11-14).
        keepOpenTrial (*undoManager, this);
        settleRedoSteps (*undoManager);
        redoStepsBefore = countRedoSteps (*undoManager);
        noteHistoryMove (*undoManager);
        undoManager->beginNewTransaction (name);
        begun = true;
        stepBegins (*undoManager, this, name);
    }

    const juce::ScopedValueSetter<bool> applyingNow (applying, true);
    change();
}

void Trial::undoHeld()
{
    if (auto* undoManager = getUndoManager())
        for (; heldSets > 0; --heldSets)
            undoStep (*undoManager);
}

void Trial::rename (const juce::String& newName)
{
    name = newName;

    if (! holding && hasChanges())
        getUndoManager()->setCurrentTransactionName (name);
}

void Trial::keep (const juce::String& keptName)
{
    if (! open)
        return;

    // Holding the history, its step is kept under the name it was given, which no step on top of it can be renamed past.
    if (! holding && hasChanges())
        getUndoManager()->setCurrentTransactionName (keptName);

    close();

    if (auto* undoManager = getUndoManager())
        stepEnds (*undoManager, this);
}

void Trial::putBack()
{
    if (! open)
        return;

    if (holding)
    {
        putBackHeld();
        return;
    }

    if (hasChanges())
        takeBackAlone (*getUndoManager(), redoStepsBefore);

    close();

    if (auto* undoManager = getUndoManager())
        stepEnds (*undoManager, this);
}

void Trial::close()
{
    open = false;
    auto& trials = getOpenTrials();
    trials.erase (std::remove (trials.begin(), trials.end(), this), trials.end());
    heldSets = heldRedo = heldAt = 0;
    stepsOnTop.clear();

    if (std::exchange (holding, false) && song != nullptr)
        song->setShowingOther (false);
}

int Trial::countStepsOnTop() const
{
    auto* undoManager = getUndoManager();
    return holding && undoManager != nullptr ? juce::jmax (0, countUndoSteps (*undoManager) - heldAt) : 0;
}

void Trial::holdHistory (Holder heldBy)
{
    jassert (open && ! hasChanges()); // before its first change

    if (! open || song == nullptr)
        return;

    holder = std::move (heldBy);
    holding = true;
    auto& undoManager = song->getUndoManager();
    settleRedoSteps (undoManager);
    heldRedo = countRedoSteps (undoManager);
    song->setShowingOther (true); // before the redo steps are done again: what hears it takes the song as it is

    // The song's redo steps done again, out of sight, so that its step goes on top of them and putBackHeld() takes everything
    // back with undos alone (JUCE workaround W56 (docs/juce-workarounds.md)).
    for (int i = 0; i < heldRedo; ++i)
        redoStep (undoManager);

    heldAt = countUndoSteps (undoManager);
}

void Trial::putBackHeld (const std::function<void()>& whileHeld)
{
    auto* undoManager = getUndoManager();

    if (! open || ! holding || undoManager == nullptr)
        return;

    const juce::ScopedValueSetter<bool> puttingBackNow (puttingBack, true); // what ends meanwhile is no edit

    // A trial with changes on top of it (Swap's), put back by its own putBack(), so that it leaves nothing begun.
    for (auto* trial : std::vector<Trial*> (getOpenTrials())) // a copy: putting one back closes it
        if (isOpenTrial (trial) && trial != this && ! trial->holding && trial->getUndoManager() == undoManager && trial->hasChanges())
            trial->putBack();

    // Steps begun on top of it and still open, taken back first (undone: what JUCE holds past the song's redo steps is none
    // of its own, below); they go on after, on the song's own.
    const auto goesOn = ! stepsOnTop.empty();
    const auto goesOnAs = stepOnTopName;

    for (auto above = countStepsOnTop(); above > 0; --above)
        undoStep (*undoManager);

    undoHeld();
    const auto ownRedoSteps = heldRedo;

    for (; heldRedo > 0; --heldRedo)
        undoStep (*undoManager);

    // The song's redo steps as they were: those it had (done again under its step, undone now), JUCE's past them none of its.
    if (auto* history = findSongHistory (*undoManager))
        history->staleRedoSteps = juce::jmax (0, countJuceRedoSteps (*undoManager) - ownRedoSteps);

    if (whileHeld != nullptr)
        whileHeld();

    close();

    if (goesOn)
        undoManager->beginNewTransaction (goesOnAs);
}

bool Trial::putBackShowing (juce::UndoManager& undoManager)
{
    const auto trials = getOpenTrials(); // a copy: putting one back closes it

    for (auto* trial : trials)
    {
        if (! isOpenTrial (trial) || ! trial->holding || trial->getUndoManager() != &undoManager)
            continue;

        // A copy: the call puts the trial back, which may destroy it. Its owner may refuse (a render has the song): then it
        // stays, and nothing is written. Only one without an owner's way is put back as it is.
        if (const auto putBackFirst = trial->holder.putBack; putBackFirst != nullptr)
            putBackFirst();
        else if (isOpenTrial (trial))
            trial->putBack();
    }

    return findHolding (undoManager) == nullptr;
}

bool Trial::makeWayForEdit (Song& song)
{
    auto& undoManager = song.getUndoManager();
    auto* held = findHolding (undoManager);

    if (held == nullptr)
        return true;

    // A copy: the call ends the hold, which may destroy the trial. Refused, the hold may have ended all the same (the version
    // kept as a step, where its branch couldn't be made): what asked was told it must be made, so it doesn't go on.
    const auto edited = held->holder.edited;
    const auto made = edited != nullptr && edited (true);
    return made && findHolding (undoManager) == nullptr;
}

bool Trial::isStepOpenOnHeld (const Song& song)
{
    const auto* held = findHolding (song.getUndoManager());
    return held != nullptr && ! held->stepsOnTop.empty();
}

bool Trial::isTrialOnHeld (const Song& song)
{
    const auto& undoManager = song.getUndoManager();

    if (findHolding (undoManager) == nullptr)
        return false;

    for (auto* trial : getOpenTrials())
        if (! trial->holding && trial->getUndoManager() == &undoManager && trial->hasChanges())
            return true;

    return false;
}

Trial* Trial::findHolding (const juce::UndoManager& undoManager)
{
    for (auto* trial : getOpenTrials())
        if (trial->holding && trial->getUndoManager() == &undoManager)
            return trial;

    return nullptr;
}

void Trial::stepBegins (juce::UndoManager& undoManager, const void* by, const juce::String& stepName)
{
    auto* held = findHolding (undoManager);

    if (held == nullptr || held->applying)
        return;

    if (held->stepsOnTop.empty())
        held->stepOnTopName = stepName;

    held->stepsOnTop.push_back (by);

    // Each step that may come on top of it kept beside the song's own (and the trial's step), so that none of them goes as
    // they come: JUCE drops steps only as one is performed, never as the limit rises (JUCE workaround W56
    // (docs/juce-workarounds.md)).
    if (auto* history = findSongHistory (undoManager))
        history->keepMoreSteps (juce::jmax (2, 1 + held->countStepsOnTop() + (int) held->stepsOnTop.size()));
}

void Trial::stepEnds (juce::UndoManager& undoManager, const void* by)
{
    auto* held = findHolding (undoManager);

    if (held == nullptr)
        return;

    auto& open = held->stepsOnTop;
    const auto found = std::find (open.begin(), open.end(), by);

    if (found == open.end())
        return;

    open.erase (found);

    // Once the last begun on top of it has ended (steps begun inside one another, which can end in another order), and
    // not as it is put back. Without changes on top of it, nothing happened: it holds on.
    if (! open.empty() || held->puttingBack || held->countStepsOnTop() == 0)
        return;

    // A copy: the call ends the hold, which may destroy the trial. Refused (an owner without a way), it is kept as it is, the
    // steps on top of it.
    const auto edited = held->holder.edited;

    if (edited != nullptr && edited (false))
        return;

    if (auto* still = findHolding (undoManager))
        still->keep (still->name);
}

void Trial::keepOpenTrial (juce::UndoManager& undoManager)
{
    keepOpenTrial (undoManager, nullptr);
}

void Trial::keepOpenTrial (juce::UndoManager& undoManager, const Trial* except)
{
    const auto trials = getOpenTrials(); // a copy: keeping one closes it

    for (auto* trial : trials)
    {
        // One whose keep is under way is kept by it, once. One that holds the history stays: what begins goes on top of it, and
        // is told to it as it ends (stepEnds()).
        if (! isOpenTrial (trial) || trial == except || trial->getUndoManager() != &undoManager || trial->applying || trial->keepPending
            || trial->holding)
            continue;

        // Without changes, it begins again at its next one, after the step beginning now.
        if (! trial->hasChanges())
        {
            trial->begun = false;
            continue;
        }

        // A copy: the call may keep the trial, which may destroy it.
        if (const auto keepWhatWasTried = trial->keepFirst; keepWhatWasTried != nullptr)
            keepWhatWasTried();

        if (isOpenTrial (trial))
            trial->keep (trial->name);
    }
}

bool Trial::isKeepPending (const Song& song)
{
    for (auto* trial : getOpenTrials())
        if (trial->getUndoManager() == &song.getUndoManager() && trial->keepPending)
            return true;

    return false;
}

bool Trial::isInProgress (const Song& song)
{
    for (auto* trial : getOpenTrials())
        if (trial->getUndoManager() == &song.getUndoManager() && (trial->holding || trial->hasChanges()))
            return true;

    return false;
}

bool Trial::isTrying (const Song& song)
{
    for (auto* trial : getOpenTrials())
        if (trial->getUndoManager() == &song.getUndoManager() && ! trial->holding && trial->hasChanges())
            return true;

    return false;
}

//==============================================================================
RecordingStep::RecordingStep (Song& song, const juce::String& name)
    : undoManager (song.getUndoManager())
{
    if (! isInGesture (undoManager))
    {
        Trial::keepOpenTrial (undoManager);
        settleRedoSteps (undoManager);
        undoManager.beginNewTransaction (name);
        begun = true;

        // On a song shown in place (a version open), what it records goes on top of what shows it, and is an edit of it as
        // it ends with changes (a branch), as any step's; one that records nothing changed nothing.
        Trial::stepBegins (undoManager, this, name);
    }
}

RecordingStep::~RecordingStep()
{
    if (begun)
        Trial::stepEnds (undoManager, this);
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
