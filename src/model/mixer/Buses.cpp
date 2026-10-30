// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Buses.h"
#include "internal/Mixer.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/mirror/internal/Mirror.h"

namespace phazite
{
bool isBus (const te::Track& track)
{
    // A folder whose volume a fader of its own sets, not a VCA (Schema.h: channelFader).
    return track.isFolderTrack() && findTrackState (track).hasProperty (schema::channelFader);
}

void makeBus (te::FolderTrack& folder)
{
    if (isBus (folder))
        return;

    // A fader of its own in place of its VCA, which the mirror makes at the folder's volume (the song's: Schema.h
    // channelVolume).
    auto& edit = folder.edit;
    auto* undo = &edit.getUndoManager();
    auto state = findTrackState (folder);
    state.removeProperty (schema::channelVCA, undo);
    state.setProperty (schema::channelFader, (juce::int64) makeID (edit), undo);
    mirror::reconcileTracks (edit);
    mirror::reconcileDevices (edit);
}

void makePlainFolder (te::FolderTrack& folder)
{
    if (! isBus (folder))
        return;

    // A VCA of its own in place of its fader, at the folder's volume; its devices go from the song too.
    auto& edit = folder.edit;
    auto* undo = &edit.getUndoManager();
    auto state = findTrackState (folder);

    for (const auto& device : getDeviceStates (state))
        state.removeChild (device, undo);

    state.removeProperty (schema::channelFader, undo);
    state.setProperty (schema::channelVCA, (juce::int64) makeID (edit), undo);
    mirror::reconcileTracks (edit);
    mirror::reconcileDevices (edit);
}

void mixAsBus (te::FolderTrack& folder)
{
    makeBus (folder);
    findTrackState (folder).setProperty (schema::folderBus, true, &folder.edit.getUndoManager());
}
} // namespace phazite
