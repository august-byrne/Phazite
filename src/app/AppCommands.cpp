// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AppCommands.h"
#include "model/arrangement/Outlines.h"
#include "model/arrangement/Sections.h"
#include "model/arrangement/Tracks.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/UndoCommands.h"
#include "model/mixer/Mixer.h"
#include "model/project/ProjectContext.h"
#include "model/transport/Metronome.h"
#include "model/transport/Playback.h"
#include "ui/playlist/TrackHeights.h"

namespace phazite
{
void addSongCommands (CommandRegistry& registry, ProjectContext& context)
{
    auto& selection = context.getSelection();
    auto& range = context.getRangeSelection();

    addUndoCommands (registry, selection);
    addTransportCommands (registry, selection);
    addSelectionCommands (registry, selection, range);
    addTrackCommands (registry, selection);
    ui::addTrackViewCommands (registry, selection); // right after the model's: the order decides key ties and lists
    addSectionCommands (registry, selection);
    addOutlineCommands (registry, selection);
    addMixerCommands (registry, selection);
    addLoopCommands (registry, selection, range);
    addMetronomeCommands (registry, selection);
}
} // namespace phazite
