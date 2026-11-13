// SPDX-License-Identifier: AGPL-3.0-or-later
// The open project's undo history as a list, for Source Control's Undo History (docs/design.md, open question 22, as Ableton
// Live 12.1's, Logic Pro's and Studio One's): its steps by their names, and a step gone back or forth to, undoing or redoing
// until it is the last one done, each as Undo and Redo do it. Message thread only.
#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace phazite
{
class ProjectContext;

struct UndoHistoryInfo
{
    std::vector<juce::String> steps; // the names of the steps the history holds, oldest first: those done, then those undone
    int done = 0;                    // how many of them, from the first, are done: Redo would do the next one again

    bool operator== (const UndoHistoryInfo&) const = default;
};

// The open project's song's history; empty while no project is open. It holds as many steps as the song's history keeps:
// the oldest go once it is full.
UndoHistoryInfo getUndoHistory (const ProjectContext&);

// Undoes or redoes until so many of the history's steps are done (none: before the first one), as Undo and Redo do it, what
// plays following. Nothing while no project is open; a number past the history's ends goes to that end.
void goToUndoStep (ProjectContext&, int done);
} // namespace phazite
