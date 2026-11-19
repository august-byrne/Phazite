// SPDX-License-Identifier: AGPL-3.0-or-later
#include "UndoHistory.h"
#include "internal/Undo.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

namespace phazite
{
UndoHistoryInfo getUndoHistory (const ProjectContext& context)
{
    UndoHistoryInfo history;
    const auto* song = findSong (context);

    if (song == nullptr)
        return history;

    // As Undo and Redo reach them: the song's own steps alone (getHistorySteps()), its own as they come back while a version is
    // open.
    const auto steps = getHistorySteps (*song);

    for (const auto& step : steps.done)
        history.steps.push_back (step);

    for (const auto& step : steps.ahead)
        history.steps.push_back (step);

    history.done = steps.done.size();
    return history;
}

void goToUndoStep (ProjectContext& context, int done)
{
    auto* song = findSong (context);

    if (song == nullptr)
        return;

    // Each a step as Undo and Redo take it, so that what plays follows each, as it follows them; what a trial tries is kept
    // first, by the first of them (undo()). Bounded by the steps there are.
    auto history = getUndoHistory (context);
    const auto target = juce::jlimit (0, (int) history.steps.size(), done);

    for (auto steps = (int) history.steps.size(); history.done > target && canUndo (*song) && steps-- > 0;)
    {
        undo (*song);
        history = getUndoHistory (context);
    }

    for (auto steps = (int) history.steps.size(); history.done < target && canRedo (*song) && steps-- > 0;)
    {
        redo (*song);
        history = getUndoHistory (context);
    }
}
} // namespace phazite
