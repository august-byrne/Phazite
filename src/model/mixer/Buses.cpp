// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Buses.h"
#include "internal/Mixer.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"

namespace phazite
{
bool isBus (const Song& song, TrackID track)
{
    // A folder whose volume a fader of its own sets, not a VCA (Schema.h: channelFader).
    const auto state = findTrackState (song, track);
    return isFolderState (state) && state.hasProperty (schema::channelFader);
}

void makeBus (Song& song, TrackID folder)
{
    if (! isFolder (song, folder) || isBus (song, folder))
        return;

    // A fader of its own in place of its VCA, which plays at the folder's volume (the song's: Schema.h channelVolume).
    auto* undo = &song.getUndoManager();
    auto state = findTrackState (song, folder);
    state.removeProperty (schema::channelVCA, undo);
    state.setProperty (schema::channelFader, (juce::int64) song.makeID(), undo);
    song.reconcile (Song::Part::tracks);
    song.reconcile (Song::Part::devices);
}

void makePlainFolder (Song& song, TrackID folder)
{
    if (! isBus (song, folder))
        return;

    // A VCA of its own in place of its fader, at the folder's volume; its devices go from the song too.
    auto* undo = &song.getUndoManager();
    auto state = findTrackState (song, folder);

    for (const auto& device : getDeviceStates (state))
        state.removeChild (device, undo);

    state.removeProperty (schema::channelFader, undo);
    state.setProperty (schema::channelVCA, (juce::int64) song.makeID(), undo);
    song.reconcile (Song::Part::tracks);
    song.reconcile (Song::Part::devices);
}

void mixAsBus (Song& song, TrackID folder)
{
    makeBus (song, folder);

    if (auto state = findTrackState (song, folder); isFolderState (state))
        state.setProperty (schema::folderBus, true, &song.getUndoManager());
}
} // namespace phazite
