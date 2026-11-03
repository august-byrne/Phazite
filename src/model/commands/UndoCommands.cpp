// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/UndoCommands.h"
#include "CommandRegistry.h"
#include "internal/Undo.h"

namespace phazite
{
void addUndoCommands (CommandRegistry& registry, Selection& selection)
{
    registry.add ({ .id = "edit.undo",
                    .name = "Undo",
                    .category = "Edit",
                    .description = "Undoes the last change.",
                    .synonyms = { "back", "revert" },
                    .perform = withSong (selection, [] (Song& song) { undo (song); }),
                    .isEnabled = ifSong (selection, [] (const Song& song) { return canUndo (song); }),
                    .currentName = [&selection] { return describeUndo (selection.getSong()); },
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "edit.redo",
                    .name = "Redo",
                    .category = "Edit",
                    .description = "Redoes the change you last undid.",
                    .synonyms = { "again" },
                    .perform = withSong (selection, [] (Song& song) { redo (song); }),
                    .isEnabled = ifSong (selection, [] (const Song& song) { return canRedo (song); }),
                    .currentName = [&selection] { return describeRedo (selection.getSong()); },
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "edit.undoOrRedo",
                    .name = "Undo or Redo",
                    .category = "Edit",
                    .description = "Undoes the last change, or redoes it if you just undid it, like Ctrl+Z in FL Studio.",
                    .synonyms = { "undo", "redo", "toggle" },
                    .perform = withSong (selection, [] (Song& song) { undoOrRedo (song); }),
                    .isEnabled = ifSong (selection, [] (const Song& song) { return canUndo (song) || canRedo (song); }),
                    .currentName = [&selection] { return describeUndoOrRedo (selection.getSong()); } });
}
} // namespace phazite
