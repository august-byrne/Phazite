// SPDX-License-Identifier: AGPL-3.0-or-later
#include "RecordingModel.h"
#include "internal/AudioRecorder.h"
#include "internal/Recording.h"
#include "internal/Take.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/SongOwnValue.h"
#include "model/base/internal/TracksValue.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/LiveInput.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>
#include <utility>

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

    // Whether the open song holds a track that can record (not a folder or the master): a track or a return, by its kind.
    bool canHoldInput (const ProjectContext& context, TrackID id)
    {
        const auto* song = findSong (context);
        const auto kind = song != nullptr && id.isValid() ? getTrackKind (*song, id) : std::nullopt;
        return kind == TrackValue::Kind::track || kind == TrackValue::Kind::returnTrack;
    }

    // The input chosen last, which comes first in the next track's list: remembered, not an undo step (outside the history,
    // whatever step is open).
    void rememberLastInput (Song& song, const juce::String& inputID)
    {
        auto outside = getSongOutside (song);
        outside.lastInput = inputID;
        setSongOutside (song, std::move (outside));
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
    return canHoldInput (context, id) ? getTrackInput (*findSong (context), id) : juce::String();
}

juce::String getLastInput (const ProjectContext& context)
{
    auto* song = findSong (context);
    return song != nullptr ? getSongOutside (*song).lastInput : juce::String();
}

void setTrackInput (ProjectContext& context, TrackID id, const juce::String& inputID)
{
    auto* song = findSong (context);

    if (song == nullptr || ! canHoldInput (context, id) || ! canRecordAudio (context, id)
        || (inputID.isNotEmpty() && getInputChannels (inputID).empty()))
        return;

    const UndoStep step (*song, "Change Input");
    setTrackFields (*song, id, [&inputID] (TrackValue& value) { value.input = inputID; });

    // The input chosen last comes first in the next track's list: remembered, not an undo step.
    if (inputID.isNotEmpty())
        rememberLastInput (*song, inputID);
}

InputMonitoring getInputMonitoring (const ProjectContext& context, TrackID id)
{
    switch (canHoldInput (context, id) ? getTrackMonitoring (*findSong (context), id) : TrackValue::Monitoring::whileArmed)
    {
        case TrackValue::Monitoring::always:
            return InputMonitoring::always;
        case TrackValue::Monitoring::never:
            return InputMonitoring::never;
        case TrackValue::Monitoring::whileArmed:
            break;
    }

    return InputMonitoring::whileArmed;
}

void setInputMonitoring (ProjectContext& context, TrackID id, InputMonitoring monitoring)
{
    auto* song = findSong (context);

    if (song == nullptr || ! canHoldInput (context, id) || ! canRecordAudio (context, id))
        return;

    const UndoStep step (*song, "Change Input Monitoring");
    const auto held = monitoring == InputMonitoring::always  ? TrackValue::Monitoring::always
                      : monitoring == InputMonitoring::never ? TrackValue::Monitoring::never
                                                             : TrackValue::Monitoring::whileArmed;
    setTrackFields (*song, id, [held] (TrackValue& value) { value.monitoring = held; });
}

bool isArmed (const ProjectContext& context, TrackID id)
{
    auto* recorder = findRecorder (context);
    return recorder != nullptr && canHoldInput (context, id) && recorder->isArmed (id);
}

void setArmed (ProjectContext& context, TrackID id, bool armed)
{
    auto* recorder = findRecorder (context);

    if (recorder != nullptr && canHoldInput (context, id) && (! armed || canRecordAudio (context, id)))
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
    // Those ended and still waiting for their clips (stopped during a drag) too, until they are placed.
    auto* session = getSession (context);
    std::vector<RecordingInProgress> shown;

    if (session != nullptr)
        for (auto* take : session->getTakesShown())
            for (auto& recording : take->getInProgress())
                shown.push_back (std::move (recording));

    return shown;
}

std::vector<float> getRecordingLevels (const ProjectContext& context, TrackID track, BeatRange span, int columns)
{
    // Each column's from the take that shows what it recorded on the track there, the latest where two do: a take ended
    // during a drag waits to be placed beside the one recording after it (a jump), each over a stretch of its own.
    auto* session = getSession (context);
    auto takes = session != nullptr ? session->getTakesShown() : std::vector<Take*>();
    std::erase_if (takes, [track] (const Take* take) { return ! take->showsOn (track); });
    std::vector<float> levels ((size_t) std::max (0, columns), 0.0f);

    if (takes.size() == 1)
        return takes.front()->getLevels (track, span, columns);

    std::vector<bool> found (levels.size(), false);
    const auto step = span.getLength().inBeats() / std::max (1, columns);

    for (auto take = takes.rbegin(); take != takes.rend(); ++take)
    {
        const auto ranges = (*take)->getInputRanges (track); // the input's alone: no notes looked through
        const auto its = (*take)->getLevels (track, span, columns);

        for (size_t column = 0; column < levels.size(); ++column)
        {
            const auto beat = BeatPosition::fromBeats (span.getStart().inBeats() + step * ((double) column + 0.5));
            const auto covers =
                std::any_of (ranges.begin(), ranges.end(), [beat] (const BeatRange& range) { return range.contains (beat); });

            if (covers && ! found[column])
            {
                levels[column] = its[column];
                found[column] = true;
            }
        }
    }

    return levels;
}

//==============================================================================
bool canRecordMidi (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);
    return song != nullptr && id.isValid() && getTrackKind (*song, id) == TrackValue::Kind::track && hasInstrument (*song, id);
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

    for (const auto id : getTrackIDs (*song))
    {
        if (isMidiArmed (context, id))
        {
            targets.push_back ({ id, RecordSource::midi, {} });
        }
        else if (recorder->isArmed (id))
        {
            const auto input = getTrackInput (*song, id);

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

bool isRecording (const ProjectContext& context)
{
    const auto* session = getSession (context);
    return session != nullptr && session->isRecording();
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

        setTrackFields (*song, track,
                        [&name, &inputID] (TrackValue& value)
                        {
                            value.name = name;
                            value.input = inputID;
                        });
    }

    // The input chosen last comes first in the next track's list, and the track records it: neither is an undo step.
    rememberLastInput (*song, inputID);
    recorder->setArmed (track, true);
    return track;
}
} // namespace phazite
