// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/LiveInput.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/PlaybackSession.h"

namespace phazite
{
namespace
{
    // A track live MIDI can play: a track, not a folder (nor the master).
    bool canPlayLive (const Song& song, TrackID track)
    {
        const auto state = findTrackState (song, track);
        return state.hasType (schema::TRACK) && ! isFolderState (state);
    }
} // namespace

TrackID getSelectedTrack (const Selection& selection)
{
    auto* song = selection.getSong();

    if (song == nullptr)
        return {};

    const auto orNone = [song] (TrackID track) { return canPlayLive (*song, track) ? track : TrackID(); };

    for (const auto track : selection.getTracks())
        if (canPlayLive (*song, track))
            return track;

    for (const auto clip : selection.getClips())
        if (const auto track = getClipTrack (*song, clip); canPlayLive (*song, track))
            return track;

    // Notes selected in the piano roll: their clip's track.
    if (const auto notes = getSelectedNotes (selection); ! notes.empty())
        return orNone (getClipTrack (*song, notes.front().clip));

    // Devices selected in the mixer: their channel's track (a container's devices: their container's).
    if (const auto devices = selection.getDevices(); ! devices.empty())
        return orNone (getDeviceChannel (*song, devices.front()));

    return {};
}

void followSelectedTrack (const Selection& selection)
{
    auto* song = selection.getSong();

    if (song == nullptr)
        return;

    if (const auto track = getSelectedTrack (selection); track.isValid() && hasInstrument (*song, track))
        setLiveMidiTrack (*song, track);
    else
        routeLiveMidiInput (*song); // in case the track it played was deleted
}

TrackID getLiveMidiTrack (const Song& song)
{
    if (const auto* session = findSession (song); session != nullptr && canPlayLive (song, session->getLiveTrack()))
        return session->getLiveTrack();

    return {};
}

bool setLiveMidiTrack (Song& song, TrackID track)
{
    auto* session = findSession (song);

    if (session == nullptr || ! canPlayLive (song, track))
        return false;

    // Without an undo step: this follows the track being played, it isn't an edit of the song.
    session->setLiveTrack (track);
    return true;
}

void routeLiveMidiInput (Song& song)
{
    auto* session = findSession (song);

    if (session == nullptr)
        return;

    // The track it plays already.
    if (getLiveMidiTrack (song).isValid())
    {
        session->followLiveMidi();
        return;
    }

    for (const auto& trackState : getTrackStates (song))
    {
        if (const auto track = getTrackID (trackState); canPlayLive (song, track) && hasInstrument (song, track))
        {
            setLiveMidiTrack (song, track);
            return;
        }
    }

    session->setLiveTrack ({});
}
} // namespace phazite
