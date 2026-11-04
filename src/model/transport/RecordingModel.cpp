// SPDX-License-Identifier: AGPL-3.0-or-later
#include "RecordingModel.h"
#include "internal/AudioRecorder.h"
#include "internal/Recording.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

namespace phazite
{
namespace
{
    // The open song's recorder, which its session owns, or nullptr.
    AudioRecorder* findRecorder (const ProjectContext& context)
    {
        auto* session = getSession (context);
        return session != nullptr ? &session->getRecorder() : nullptr;
    }

    // A track's element in the open song, if it is a track (not a folder or the master), or else invalid.
    juce::ValueTree findTrack (const ProjectContext& context, TrackID id)
    {
        auto* song = findSong (context);
        const auto state = song != nullptr && id.isValid() ? findTrackState (*song, id) : juce::ValueTree();
        return state.hasType (schema::TRACK) && ! isFolderState (state) ? state : juce::ValueTree();
    }
} // namespace

std::vector<AudioInput> getAudioInputs (const ProjectContext& context)
{
    std::vector<AudioInput> inputs;
    const auto* recorder = findRecorder (context);
    const auto numChannels = recorder != nullptr ? recorder->getNumInputs() : 0;

    for (int channel = 1; channel <= numChannels; ++channel)
        inputs.push_back ({ juce::String (channel), describeInput (juce::String (channel)), 1 });

    for (int channel = 1; channel + 1 <= numChannels; channel += 2)
    {
        const auto id = juce::String (channel) + "+" + juce::String (channel + 1);
        inputs.push_back ({ id, describeInput (id), 2 });
    }

    return inputs;
}

juce::String describeInput (const juce::String& inputID)
{
    return getInputChannels (inputID).empty() ? juce::String ("No input") : "In " + inputID;
}

bool canRecordAudio (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);
    return song != nullptr && canRecordAudio (*song, id);
}

juce::String getTrackInput (const ProjectContext& context, TrackID id)
{
    return findTrack (context, id).getProperty (schema::trackInput).toString();
}

juce::String getLastInput (const ProjectContext& context)
{
    auto* song = findSong (context);
    return song != nullptr ? song->getState().getProperty (schema::lastInput).toString() : juce::String();
}

void setTrackInput (ProjectContext& context, TrackID id, const juce::String& inputID)
{
    auto* song = findSong (context);
    auto track = findTrack (context, id);

    if (song == nullptr || ! track.isValid() || ! canRecordAudio (context, id)
        || (inputID.isNotEmpty() && getInputChannels (inputID).empty()))
        return;

    const UndoStep step (*song, "Change Input");

    if (inputID.isEmpty())
        track.removeProperty (schema::trackInput, &song->getUndoManager());
    else
        track.setProperty (schema::trackInput, inputID, &song->getUndoManager());

    // The input chosen last comes first in the next track's list: remembered, not an undo step.
    if (inputID.isNotEmpty())
        song->getState().setProperty (schema::lastInput, inputID, nullptr);
}

InputMonitoring getInputMonitoring (const ProjectContext& context, TrackID id)
{
    const auto value = findTrack (context, id).getProperty (schema::trackMonitoring).toString();
    return value == schema::monitorAlways  ? InputMonitoring::always
           : value == schema::monitorNever ? InputMonitoring::never
                                           : InputMonitoring::whileArmed;
}

void setInputMonitoring (ProjectContext& context, TrackID id, InputMonitoring monitoring)
{
    auto* song = findSong (context);
    auto track = findTrack (context, id);

    if (song == nullptr || ! track.isValid() || ! canRecordAudio (context, id))
        return;

    const UndoStep step (*song, "Change Input Monitoring");
    auto* undoManager = &song->getUndoManager();

    if (monitoring == InputMonitoring::whileArmed)
        track.removeProperty (schema::trackMonitoring, undoManager);
    else
        track.setProperty (schema::trackMonitoring, monitoring == InputMonitoring::always ? schema::monitorAlways : schema::monitorNever,
                           undoManager);
}

bool isArmed (const ProjectContext& context, TrackID id)
{
    auto* recorder = findRecorder (context);
    return recorder != nullptr && findTrack (context, id).isValid() && recorder->isArmed (id);
}

void setArmed (ProjectContext& context, TrackID id, bool armed)
{
    auto* recorder = findRecorder (context);

    if (recorder != nullptr && findTrack (context, id).isValid() && (! armed || canRecordAudio (context, id)))
        recorder->setArmed (id, armed);
}

void armOnly (ProjectContext& context, TrackID id)
{
    for (const auto other : getArmedTracks (context))
        if (other != id)
            setArmed (context, other, false);

    setArmed (context, id, true);
}

std::vector<TrackID> getArmedTracks (const ProjectContext& context)
{
    auto* recorder = findRecorder (context);
    return recorder != nullptr ? recorder->getArmedTracks() : std::vector<TrackID>();
}

bool isPunchOn (const ProjectContext& context)
{
    auto* song = findSong (context);
    return song != nullptr && isPunchOn (*song);
}

bool canPunch (const ProjectContext& context)
{
    auto* song = findSong (context);
    return song != nullptr && canPunch (*song);
}

void setPunchOn (ProjectContext& context, bool on)
{
    if (auto* song = findSong (context))
        setPunchOn (*song, on);
}

ClipTakes getClipTakes (const ProjectContext& context, ClipID id)
{
    auto* song = findSong (context);

    if (song == nullptr || getClipTakeCount (*song, id) < 2)
        return {};

    return { getClipTakeCount (*song, id), getClipTake (*song, id) };
}

void setClipTake (ProjectContext& context, ClipID id, int take)
{
    auto* song = findSong (context);
    const auto takes = getClipTakes (context, id);

    if (song == nullptr || takes.count < 2 || take < 1 || take > takes.count || take == takes.playing)
        return;

    const UndoStep step (*song, "Choose Take");
    playTake (*song, id, take);
}
} // namespace phazite
