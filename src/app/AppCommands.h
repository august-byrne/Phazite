// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "model/arrangement/Selection.h"
#include "model/engine/EngineSetup.h"

namespace phazite
{
class CommandRegistry;

// The commands that work on the open song and need no window, in the order the app registers them, which decides
// which of two commands on the same key runs, the order of the palette's equal matches and of the shortcuts list:
// undo, redo and the transport, then the selection's, the tracks', the sections', the mixer's and the loop's. The
// project's own commands are ProjectSession's; the panels' are the UI's.
void addSongCommands (CommandRegistry&, te::SelectionManager&, RangeSelection&, UIBehaviour&);
} // namespace phazite
