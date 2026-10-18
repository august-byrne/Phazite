// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AppCommands.h"
#include "model/arrangement/internal/Outlines.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/chords/ChordModel.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/UndoCommands.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Capture.h"
#include "model/transport/internal/Metronome.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"
#include "ui/playlist/TrackHeights.h"

namespace phazite
{
void addSongCommands (CommandRegistry& registry, ProjectContext& context)
{
    auto& selection = getSelection (context);
    auto& range = getRangeSelection (context);

    addUndoCommands (registry, selection);
    addTransportCommands (registry, selection);
    addSelectionCommands (registry, selection, range);
    addTrackCommands (registry, selection);
    ui::addTrackViewCommands (registry, context); // right after the model's: the order decides key ties and lists
    addSectionCommands (registry, selection);
    addOutlineCommands (registry, selection);
    addChordCommands (registry, context);
    addMixerCommands (registry, selection);
    addContainerCommands (registry, selection);
    addAutomationCommands (registry, context);
    addLoopCommands (registry, selection, range);
    addMetronomeCommands (registry, selection);
    addRecordingCommands (registry, selection);
    addCaptureCommands (registry, selection);
}
} // namespace phazite
