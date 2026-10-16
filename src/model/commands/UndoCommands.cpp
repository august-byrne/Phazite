// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/UndoCommands.h"
#include "CommandRegistry.h"
#include "internal/Undo.h"

namespace phazite
{
void addUndoCommands (CommandRegistry& registry, te::SelectionManager& selection)
{
    registry.add ({ .id = "edit.undo",
                    .name = "Undo",
                    .category = "Edit",
                    .description = "Undoes the last change.",
                    .synonyms = { "back", "revert" },
                    .perform = withEdit (selection, undo),
                    .isEnabled = ifEdit (selection, canUndo),
                    .currentName = [&selection] { return describeUndo (selection.getEdit()); },
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "edit.redo",
                    .name = "Redo",
                    .category = "Edit",
                    .description = "Redoes the change you last undid.",
                    .synonyms = { "again" },
                    .perform = withEdit (selection, redo),
                    .isEnabled = ifEdit (selection, canRedo),
                    .currentName = [&selection] { return describeRedo (selection.getEdit()); },
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "edit.undoOrRedo",
                    .name = "Undo or Redo",
                    .category = "Edit",
                    .description = "Undoes the last change, or redoes it if you just undid it, like Ctrl+Z in FL Studio.",
                    .synonyms = { "undo", "redo", "toggle" },
                    .perform = withEdit (selection, undoOrRedo),
                    .isEnabled = ifEdit (selection, [] (te::Edit& edit) { return canUndo (edit) || canRedo (edit); }),
                    .currentName = [&selection] { return describeUndoOrRedo (selection.getEdit()); } });
}
} // namespace phazite
