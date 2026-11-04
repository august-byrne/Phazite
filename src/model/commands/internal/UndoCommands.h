// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "model/base/internal/Selection.h"

namespace phazite
{
class CommandRegistry;

// Undo, redo and FL Studio's undo-or-redo, for the selection's song.
void addUndoCommands (CommandRegistry&, Selection&);
} // namespace phazite
