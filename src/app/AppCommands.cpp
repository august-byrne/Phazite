// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AppCommands.h"
#include "model/arrangement/Outlines.h"
#include "model/arrangement/Sections.h"
#include "model/arrangement/Tracks.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/UndoCommands.h"
#include "model/mixer/Mixer.h"
#include "model/transport/Playback.h"

namespace phazite
{
void addSongCommands (CommandRegistry& registry, te::SelectionManager& selection, RangeSelection& range)
{
    addUndoCommands (registry, selection);
    addTransportCommands (registry, selection);
    addSelectionCommands (registry, selection, range);
    addTrackCommands (registry, selection);
    addSectionCommands (registry, selection);
    addOutlineCommands (registry, selection);
    addMixerCommands (registry, selection);
    addLoopCommands (registry, selection, range);
}
} // namespace phazite
