// SPDX-License-Identifier: AGPL-3.0-or-later
#include "RecordingModel.h"
#include "internal/AudioRecorder.h"
#include "internal/Recording.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/LiveInput.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

namespace phazite
{
namespace
{
    te::AudioTrack* findTrack (const ProjectContext& context, TrackID id)
    {
        auto* edit = getEdit (context);
        return edit != nullptr && id.isValid() ? dynamic_cast<te::AudioTrack*> (te::findTrackForID (*edit, toTracktion (id))) : nullptr;
    }

    AudioRecorder* findRecorder (const ProjectContext& context)
    {
        auto* edit = getEdit (context);
        return edit != nullptr ? AudioRecorder::findFor (*edit) : nullptr;
    }

    te::AudioClipBase* findAudioClip (const ProjectContext& context, ClipID id)
    {
        auto* edit = getEdit (context);
        return edit != nullptr && id.isValid() ? dynamic_cast<te::AudioClipBase*> (te::findClipForID (*edit, toTracktion (id))) : nullptr;
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
    auto* track = findTrack (context, id);
    return track != nullptr && ! isReturnTrack (*track) && ! hasInstrument (*track);
}

juce::String getTrackInput (const ProjectContext& context, TrackID id)
{
    auto* track = findTrack (context, id);
    return track != nullptr ? findTrackState (*track).getProperty (schema::trackInput).toString() : juce::String();
}

juce::String getLastInput (const ProjectContext& context)
{
    auto* edit = getEdit (context);
    return edit != nullptr ? findPhaziteState (*edit).getProperty (schema::lastInput).toString() : juce::String();
}

void setTrackInput (ProjectContext& context, TrackID id, const juce::String& inputID)
{
    auto* track = findTrack (context, id);

    if (track == nullptr || ! canRecordAudio (context, id) || (inputID.isNotEmpty() && getInputChannels (inputID).empty()))
        return;

    const UndoStep step (track->edit, "Change Input");

    if (inputID.isEmpty())
        findTrackState (*track).removeProperty (schema::trackInput, &getSong (track->edit).getUndoManager());
    else
        findTrackState (*track).setProperty (schema::trackInput, inputID, &getSong (track->edit).getUndoManager());

    // The input chosen last comes first in the next track's list: remembered, not an undo step.
    if (inputID.isNotEmpty())
        getSong (track->edit).getState().setProperty (schema::lastInput, inputID, nullptr);
}

InputMonitoring getInputMonitoring (const ProjectContext& context, TrackID id)
{
    const auto* track = findTrack (context, id);
    const auto value = track != nullptr ? findTrackState (*track).getProperty (schema::trackMonitoring).toString() : juce::String();
    return value == schema::monitorAlways  ? InputMonitoring::always
           : value == schema::monitorNever ? InputMonitoring::never
                                           : InputMonitoring::whileArmed;
}

void setInputMonitoring (ProjectContext& context, TrackID id, InputMonitoring monitoring)
{
    auto* track = findTrack (context, id);

    if (track == nullptr || ! canRecordAudio (context, id))
        return;

    const UndoStep step (track->edit, "Change Input Monitoring");
    auto* undoManager = &getSong (track->edit).getUndoManager();

    if (monitoring == InputMonitoring::whileArmed)
        findTrackState (*track).removeProperty (schema::trackMonitoring, undoManager);
    else
        findTrackState (*track).setProperty (
            schema::trackMonitoring, monitoring == InputMonitoring::always ? schema::monitorAlways : schema::monitorNever, undoManager);
}

bool isArmed (const ProjectContext& context, TrackID id)
{
    auto* recorder = findRecorder (context);
    auto* track = findTrack (context, id);
    return recorder != nullptr && track != nullptr && recorder->isArmed (*track);
}

void setArmed (ProjectContext& context, TrackID id, bool armed)
{
    auto* recorder = findRecorder (context);
    auto* track = findTrack (context, id);

    if (recorder != nullptr && track != nullptr && (! armed || canRecordAudio (context, id)))
        recorder->setArmed (*track, armed);
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
    std::vector<TrackID> tracks;

    if (auto* recorder = findRecorder (context))
        for (auto* track : recorder->getArmedTracks())
            tracks.push_back (getTrackID (*track));

    return tracks;
}

bool isPunchOn (const ProjectContext& context)
{
    auto* edit = getEdit (context);
    return edit != nullptr && isPunchOn (*edit);
}

bool canPunch (const ProjectContext& context)
{
    auto* edit = getEdit (context);
    return edit != nullptr && canPunch (*edit);
}

void setPunchOn (ProjectContext& context, bool on)
{
    if (auto* edit = getEdit (context))
        setPunchOn (*edit, on);
}

ClipTakes getClipTakes (const ProjectContext& context, ClipID id)
{
    const auto* clip = findAudioClip (context, id);

    if (clip == nullptr || getClipTakeCount (*clip) < 2)
        return {};

    return { getClipTakeCount (*clip), getClipTake (*clip) };
}

void setClipTake (ProjectContext& context, ClipID id, int take)
{
    auto* clip = findAudioClip (context, id);
    const auto takes = getClipTakes (context, id);

    if (clip == nullptr || takes.count < 2 || take < 1 || take > takes.count || take == takes.playing)
        return;

    const UndoStep step (clip->edit, "Choose Take");
    playTake (*clip, take);
}
} // namespace phazite
