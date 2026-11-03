// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/LiveInput.h"
#include "internal/AudioIO.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/devices/internal/Devices.h"
#include "model/mirror/internal/Bridge.h"
#include "model/mixer/internal/Containers.h"
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
    if (auto* track = selection.getFirstItemOfType<te::AudioTrack>())
        return track;

    if (auto* clip = selection.getFirstItemOfType<te::Clip>())
        return dynamic_cast<te::AudioTrack*> (clip->getTrack());

    // Notes selected in the piano roll: their clip's track.
    if (const auto notes = getSelectedNotes (selection); ! notes.empty())
        return dynamic_cast<te::AudioTrack*> (notes.front().clip->getTrack());

    // Devices selected in the mixer: their channel's track (a container's devices: their container's).
    if (auto* device = selection.getFirstItemOfType<te::Plugin>())
    {
        auto* container = findContainerOf (*device);
        return dynamic_cast<te::AudioTrack*> (container != nullptr ? container->getOwnerTrack() : device->getOwnerTrack());
    }

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