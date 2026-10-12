// SPDX-License-Identifier: AGPL-3.0-or-later
// The commands that act on whatever is selected, clips or tracks: delete, duplicate, select all and
// deselect all.
#pragma once

#include "Commands.h"

namespace phazite
{
// "Delete Clip", "Delete 3 Tracks", or the verb alone when nothing is selected.
juce::String describeSelection (const te::SelectionManager&, const juce::String& verb);

// The selection commands, for the selection manager's Edit.
void addSelectionCommands (CommandRegistry&, te::SelectionManager&);
} // namespace phazite
