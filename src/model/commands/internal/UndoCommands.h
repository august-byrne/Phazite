// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "model/base/internal/Tracktion.h"

namespace phazite
{
class CommandRegistry;

// Undo, redo and FL Studio's undo-or-redo, for the selection manager's Edit.
void addUndoCommands (CommandRegistry&, te::SelectionManager&);
} // namespace phazite
