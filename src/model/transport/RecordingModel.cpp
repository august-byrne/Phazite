// SPDX-License-Identifier: AGPL-3.0-or-later
#include "RecordingModel.h"
#include "internal/AudioRecorder.h"
#include "internal/Recording.h"
#include "internal/Take.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/LiveInput.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>

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

//==============================================================================
std::vector<RecordingInProgress> getRecordingsInProgress (const ProjectContext& context)
{
    auto* session = getSession (context);
    auto* take = session != nullptr ? session->getTake() : nullptr;
    return take != nullptr ? take->getInProgress() : std::vector<RecordingInProgress>();
}

std::vector<float> getRecordingLevels (const ProjectContext& context, TrackID track, BeatRange span, int columns)
{
    auto* session = getSession (context);
    const auto* take = session != nullptr ? session->getTake() : nullptr;
    return take != nullptr ? take->getLevels (track, span, columns) : std::vector<float> ((size_t) std::max (0, columns), 0.0f);
}

//==============================================================================
bool canRecordMidi (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);
    const auto track = findTrack (context, id);
    return song != nullptr && track.isValid() && ! isReturnState (track) && hasInstrument (*song, id);
}

bool isMidiArmed (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);
    const auto* session = getSession (context);
    return song != nullptr && session != nullptr && id.isValid() && getLiveMidiTrack (*song) == id && session->recordsLiveTrack();
}

void setMidiArmed (ProjectContext& context, TrackID id, bool armed)
{
    auto* song = findSong (context);
    auto* session = getSession (context);

    if (song == nullptr || session == nullptr || ! canRecordMidi (context, id))
        return;

    // Armed, live MIDI plays it from now on and records onto it; disarmed, it plays on and records nothing.
    if (armed && getLiveMidiTrack (*song) != id)
        setLiveMidiTrack (*song, id);

    if (getLiveMidiTrack (*song) == id)
        session->setRecordsLiveTrack (armed);
}

std::vector<RecordTarget> getRecordTargets (const ProjectContext& context)
{
    std::vector<RecordTarget> targets;
    auto* song = findSong (context);
    const auto* recorder = findRecorder (context);

    if (song == nullptr || recorder == nullptr)
        return targets;

    const auto numInputs = recorder->getNumInputs();

    for (const auto& trackState : getTrackStates (*song))
    {
        const auto id = getTrackID (trackState);

        if (isMidiArmed (context, id))
        {
            targets.push_back ({ id, RecordSource::midi, {} });
        }
        else if (recorder->isArmed (id))
        {
            const auto input = trackState[schema::trackInput].toString();

            if (const auto channels = getInputChannels (input); ! channels.empty() && channels.back() < numInputs)
                targets.push_back ({ id, RecordSource::input, input });
        }
    }

    return targets;
}

juce::String describeRecordTargets (const ProjectContext& context)
{
    juce::StringArray parts;

    if (auto* song = findSong (context))
        for (const auto& target : getRecordTargets (context))
            parts.add (getTrackName (*song, target.track) + " ("
                       + (target.source == RecordSource::midi ? juce::String ("MIDI") : describeInput (target.input)) + ")");

    // "A", "A and B", "A, B and C".
    if (parts.size() < 2)
        return parts.joinIntoString ({});

    return parts.joinIntoString (", ", 0, parts.size() - 1) + " and " + parts[parts.size() - 1];
}

bool isRecordingOnto (const ProjectContext& context, TrackID id)
{
    auto* session = getSession (context);
    const auto* take = session != nullptr ? session->getTake() : nullptr;
    return take != nullptr && take->recordsOnto (id);
}

TrackID addTrackForInput (ProjectContext& context, const juce::String& inputID)
{
    auto* song = findSong (context);
    auto* recorder = findRecorder (context);
    const auto channels = getInputChannels (inputID);

    if (song == nullptr || recorder == nullptr || channels.empty() || channels.back() >= recorder->getNumInputs())
        return {};

    TrackID track;

    {
        const UndoStep step (*song, "Add Audio Track");
        auto& selection = getSelection (context);
        const auto selected = getSelectedTracks (selection);
        track = insertTrack (*song, selected.empty() ? TrackID() : selected.back(), &selection);

        if (! track.isValid())
            return {};

        // "Audio", or the first of "Audio 2", "Audio 3"... that no track is called yet.
        juce::StringArray names;

        for (const auto other : getTrackIDs (*song))
            names.add (getTrackName (*song, other));

        auto name = juce::String ("Audio");

        for (int number = 2; names.contains (name); ++number)
            name = "Audio " + juce::String (number);

        setTrackName (*song, track, name);
        findTrackState (*song, track).setProperty (schema::trackInput, inputID, &song->getUndoManager());
    }

    // The input chosen last comes first in the next track's list, and the track records it: neither is an undo step.
    song->getState().setProperty (schema::lastInput, inputID, nullptr);
    recorder->setArmed (track, true);
    return track;
}
} // namespace phazite
