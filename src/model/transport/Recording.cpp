// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Recording.h"
#include "internal/AudioRecorder.h"
#include "internal/Metronome.h"
#include "internal/Playback.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/Toggles.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/AudioIO.h"
#include "model/engine/internal/LiveInput.h"
#include "model/mirror/internal/Bridge.h"
#include "model/mirror/internal/Mirror.h"
#include "model/playback/internal/PlaybackSession.h"

namespace phazite
{
bool canRecord (te::Edit& edit)
{
    const auto* recorder = AudioRecorder::findFor (edit);
    return findPlayingSession (edit) != nullptr && (getLiveMidiTrack (edit) != nullptr || (recorder != nullptr && recorder->canRecord()));
}

bool isRecording (const te::Edit& edit)
{
    const auto* session = findSession (edit);
    return session != nullptr && session->isRecording();
}

void startRecording (te::Edit& edit)
{
    if (isRecording (edit) || ! canRecord (edit))
        return;

    // Phazite's engine records what it plays (Take.h): from where playback starts, after the count-in, or from where the
    // song is if it plays already.
    auto* session = findPlayingSession (edit);
    const auto playing = session->isPlaying();

    if (session->startTake() && ! playing)
    {
        session->locate (fromTracktion (edit.tempoSequence.toBeats (getPlayStart (edit))));
        session->play (isCountInOn (edit));
    }
}

void stopRecording (te::Edit& edit)
{
    if (isRecording (edit))
        stopAndReturn (edit);
}

void toggleRecording (te::Edit& edit)
{
    if (isRecording (edit))
        stopRecording (edit);
    else
        startRecording (edit);
}

bool canToggleRecording (te::Edit& edit)
{
    return isRecording (edit) || canRecord (edit);
}

//==============================================================================
bool isOverdubOn (te::Engine& engine)
{
    const auto* audio = AudioIO::findFor (engine);
    return audio == nullptr || audio->isOverdubOn();
}

void setOverdubOn (te::Engine& engine, bool on)
{
    if (auto* audio = AudioIO::findFor (engine))
        audio->setOverdubOn (on);
}

void toggleOverdub (te::Engine& engine)
{
    setOverdubOn (engine, ! isOverdubOn (engine));
}

bool isRecordQuantizeOn (te::Engine& engine)
{
    const auto* audio = AudioIO::findFor (engine);
    return audio != nullptr && audio->isRecordQuantizeOn();
}

void setRecordQuantizeOn (te::Engine& engine, bool on)
{
    if (auto* audio = AudioIO::findFor (engine))
        audio->setRecordQuantizeOn (on);
}
void toggleRecordQuantize (te::Engine& engine)
{
    setRecordQuantizeOn (engine, ! isRecordQuantizeOn (engine));
}

//==============================================================================
bool isPunchOn (const te::Edit& edit)
{
    return (bool) findPhaziteState (edit).getProperty (schema::punch, false);
}

bool canPunch (const te::Edit& edit)
{
    return isLoopOn (edit) && ! doesLoopFollowSong (edit);
}

void setPunchOn (te::Edit& edit, bool on)
{
    getSong (edit).getState().setProperty (schema::punch, on, nullptr); // like the loop's switch, not an undo step
}

bool isPunching (const te::Edit& edit)
{
    return isPunchOn (edit) && canPunch (edit);
}

void togglePunch (te::Edit& edit)
{
    setPunchOn (edit, ! isPunchOn (edit));
}

//==============================================================================
namespace
{
    // The selected tracks with an input, which arming arms.
    juce::Array<te::AudioTrack*> getArmableTracks (const Selection& selection)
    {
        juce::Array<te::AudioTrack*> tracks;
        auto* recorder = selection.getEdit() != nullptr ? AudioRecorder::findFor (*selection.getEdit()) : nullptr;

        if (recorder != nullptr)
            for (const auto id : selection.getTracks())
                if (auto* track = dynamic_cast<te::AudioTrack*> (mirror::findTrack (*selection.getEdit(), id)))
                    if (! getInputChannels (findTrackState (*track)[schema::trackInput].toString()).empty() && ! hasInstrument (*track))
                        tracks.add (track);

        return tracks;
    }

    // The selected audio clips with takes.
    std::vector<ClipID> getClipsWithTakes (const Selection& selection)
    {
        std::vector<ClipID> clips;

        if (auto* song = selection.getSong())
            for (const auto clip : getSelectedClips (selection))
                if (getClipTakeCount (*song, clip) > 1)
                    clips.push_back (clip);

        return clips;
    }
} // namespace

bool canArmSelectedTracks (const Selection& selection)
{
    return ! getArmableTracks (selection).isEmpty();
}

bool areSelectedTracksArmed (const Selection& selection)
{
    const auto tracks = getArmableTracks (selection);
    auto* recorder = selection.getEdit() != nullptr ? AudioRecorder::findFor (*selection.getEdit()) : nullptr;
    return recorder != nullptr && areAllOn (tracks, [recorder] (te::AudioTrack* track) { return recorder->isArmed (*track); });
}

void toggleArmSelectedTracks (const Selection& selection)
{
    if (auto* recorder = selection.getEdit() != nullptr ? AudioRecorder::findFor (*selection.getEdit()) : nullptr)
        toggleAll (
            getArmableTracks (selection), [recorder] (te::AudioTrack* track) { return recorder->isArmed (*track); },
            [recorder] (te::AudioTrack* track, bool on) { recorder->setArmed (*track, on); });
}

bool canStepTakes (const Selection& selection)
{
    return ! getClipsWithTakes (selection).empty();
}

void stepTakes (const Selection& selection, int direction)
{
    // The takes lie one after another in the clip's file, each as long as the range they were recorded round.
    if (auto* song = selection.getSong())
        for (const auto clip : getClipsWithTakes (selection))
            playTake (*song, clip, juce::jlimit (1, getClipTakeCount (*song, clip), getClipTake (*song, clip) + direction));
}

//==============================================================================
void addRecordingCommands (CommandRegistry& registry, Selection& selection)
{
    registry.add ({ .id = "transport.record",
                    .name = "Record",
                    .category = "Transport",
                    .description = "Records what you play onto the track live MIDI plays, and the armed tracks' audio inputs, from where "
                                   "playback starts, or stops recording.",
                    .synonyms = { "rec", "take", "record midi", "record audio" },
                    .perform = withEdit (selection, toggleRecording),
                    .isEnabled = ifEdit (selection, canToggleRecording),
                    .isTicked = ifEdit (selection, isRecording) });

    registry.add ({ .id = "transport.overdub",
                    .name = "Overdub",
                    .category = "Transport",
                    .description = "Recording adds what you play to the clips already there; off, it takes their place.",
                    .synonyms = { "merge recordings", "replace", "blend", "record mode" },
                    .perform = [&selection] { toggleOverdub (selection.engine); },
                    .isTicked = [&selection] { return isOverdubOn (selection.engine); } });

    registry.add ({ .id = "transport.quantizeRecordings",
                    .name = "Quantize Recordings",
                    .category = "Transport",
                    .description = "Moves the notes you record to the nearest sixteenth as they go in.",
                    .synonyms = { "input quantize", "record quantize", "snap recording", "timing" },
                    .perform = [&selection] { toggleRecordQuantize (selection.engine); },
                    .isTicked = [&selection] { return isRecordQuantizeOn (selection.engine); } });

    registry.add ({ .id = "transport.punch",
                    .name = "Punch In",
                    .category = "Transport",
                    .description = "Record keeps only what is played in the looped range, in place of what was there, cross-fading at its "
                                   "edges. Loop a range to punch in.",
                    .synonyms = { "punch in", "punch out", "auto punch", "replace a part", "drop in" },
                    .perform = withEdit (selection, togglePunch),
                    .isEnabled = ifEdit (selection, canPunch),
                    .isTicked = ifEdit (selection, isPunching) });

    registry.add ({ .id = "track.arm",
                    .name = "Arm Track",
                    .category = "Track",
                    .description =
                        "Arms the selected tracks to record their audio inputs when Record is pressed, or disarms them. Choose a "
                        "track's input with a right-click on its arm button.",
                    .synonyms = { "record enable", "rec arm", "arm", "record audio", "microphone", "input" },
                    .perform = [&selection] { toggleArmSelectedTracks (selection); },
                    .isEnabled = [&selection] { return canArmSelectedTracks (selection); },
                    .isTicked = [&selection] { return areSelectedTracksArmed (selection); } });

    registry.add (
        { .id = "clip.previousTake",
          .name = "Previous Take",
          .category = "Clip",
          .description = "Plays the take recorded before the one the selected clips play, of those recorded round a looped range.",
          .synonyms = { "take", "comp", "earlier take", "loop recording" },
          .perform = [&selection] { stepTakes (selection, -1); },
          .isEnabled = [&selection] { return canStepTakes (selection); },
          .undoTarget = undoTargetOf (selection) });

    registry.add ({ .id = "clip.nextTake",
                    .name = "Next Take",
                    .category = "Clip",
                    .description = "Plays the take recorded after the one the selected clips play, of those recorded round a looped range.",
                    .synonyms = { "take", "comp", "later take", "loop recording" },
                    .perform = [&selection] { stepTakes (selection, 1); },
                    .isEnabled = [&selection] { return canStepTakes (selection); },
                    .undoTarget = undoTargetOf (selection) });
}
} // namespace phazite
