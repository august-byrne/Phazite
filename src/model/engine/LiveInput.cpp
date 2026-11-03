// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/LiveInput.h"
#include "internal/AudioIO.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/devices/internal/Devices.h"
#include "model/mirror/internal/Bridge.h"
#include "model/mirror/internal/Mirror.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/PlaybackSession.h"

namespace phazite
{
bool hasInstrument (const te::AudioTrack& track)
{
    for (auto* plugin : track.pluginList)
        if (isInstrument (*plugin))
            return true;

    return false;
}

te::AudioTrack* getSelectedAudioTrack (const Selection& selection)
{
    auto* song = selection.getSong();
    auto* edit = selection.getEdit();

    if (song == nullptr || edit == nullptr)
        return nullptr;

    const auto findAudioTrack = [edit] (TrackID id) { return dynamic_cast<te::AudioTrack*> (mirror::findTrack (*edit, id)); };

    for (const auto track : selection.getTracks())
        if (auto* found = findAudioTrack (track))
            return found;

    for (const auto clip : selection.getClips())
        if (auto* found = findAudioTrack (getClipTrack (*song, clip)))
            return found;

    // Notes selected in the piano roll: their clip's track.
    if (const auto notes = getSelectedNotes (selection); ! notes.empty())
        return findAudioTrack (getClipTrack (*song, notes.front().clip));

    // Devices selected in the mixer: their channel's track (a container's devices: their container's).
    if (const auto devices = selection.getDevices(); ! devices.empty())
        return findAudioTrack (getDeviceChannel (*song, devices.front()));

    return nullptr;
}

void followSelectedTrack (const Selection& selection)
{
    if (auto* track = getSelectedAudioTrack (selection); track != nullptr && hasInstrument (*track))
        setLiveMidiTrack (*track);
    else if (auto* edit = selection.getEdit())
        routeLiveMidiInput (*edit); // in case the track it played was deleted
}

void playLiveMidi (te::Engine& engine, const juce::MidiMessage& message)
{
    if (auto* audio = AudioIO::findFor (engine); audio != nullptr && (message.isNoteOn() || message.isNoteOff()))
        audio->playLive (message);
}

te::AudioTrack* getLiveMidiTrack (te::Edit& edit)
{
    if (const auto* session = findSession (edit); session != nullptr && session->getLiveTrack().isValid())
        return dynamic_cast<te::AudioTrack*> (te::findTrackForID (edit, toTracktion (session->getLiveTrack())));

    return nullptr;
}

bool setLiveMidiTrack (te::AudioTrack& track)
{
    auto* session = findSession (track.edit);

    if (session == nullptr)
        return false;

    // Without an undo step: this follows the track being played, it isn't an edit of the song.
    session->setLiveTrack (getTrackID (findTrackState (track)));
    return true;
}

void routeLiveMidiInput (te::Edit& edit)
{
    auto* session = findSession (edit);

    if (session == nullptr)
        return;

    // The track it plays already.
    if (getLiveMidiTrack (edit) != nullptr)
    {
        session->followLiveMidi();
        return;
    }

    for (auto* track : te::getAudioTracks (edit))
    {
        if (hasInstrument (*track))
        {
            setLiveMidiTrack (*track);
            return;
        }
    }

    session->setLiveTrack ({});
}
} // namespace phazite