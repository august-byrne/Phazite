// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Recording.h"
#include "internal/AudioRecorder.h"
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
#include "model/playback/internal/PlaybackSession.h"

namespace phazite
{
bool canRecord (const Song& song)
{
    const auto* session = findPlayingSession (song);
    return session != nullptr && (getLiveMidiTrack (song).isValid() || session->getRecorder().canRecord());
}

bool isRecording (const Song& song)
{
    const auto* session = findSession (song);
    return session != nullptr && session->isRecording();
}

void startRecording (Song& song)
{
    if (isRecording (song) || ! canRecord (song))
        return;

    // Phazite's engine records what it plays (Take.h): from where playback starts, after the count-in, or from where the
    // song is if it plays already.
    auto* session = findPlayingSession (song);
    const auto playing = session->isPlaying();

    if (session->startTake() && ! playing)
    {
        session->locate (getPlayStartBeat (song));
        session->play (session->getAudioIO().isCountInOn());
    }
}

void stopRecording (Song& song)
{
    if (isRecording (song))
        stopAndReturn (song);
}

void toggleRecording (Song& song)
{
    if (isRecording (song))
        stopRecording (song);
    else
        startRecording (song);
}

bool canToggleRecording (const Song& song)
{
    return isRecording (song) || canRecord (song);
}

//==============================================================================
bool isOverdubOn (const AudioIO& audio)
{
    return audio.isOverdubOn();
}

void setOverdubOn (AudioIO& audio, bool on)
{
    audio.setOverdubOn (on);
}

void toggleOverdub (AudioIO& audio)
{
    setOverdubOn (audio, ! isOverdubOn (audio));
}

bool isRecordQuantizeOn (const AudioIO& audio)
{
    return audio.isRecordQuantizeOn();
}

void setRecordQuantizeOn (AudioIO& audio, bool on)
{
    audio.setRecordQuantizeOn (on);
}

void toggleRecordQuantize (AudioIO& audio)
{
    setRecordQuantizeOn (audio, ! isRecordQuantizeOn (audio));
}

//==============================================================================
bool isPunchOn (const Song& song)
{
    return (bool) song.getState().getProperty (schema::punch, false);
}

bool canPunch (const Song& song)
{
    return isLoopOn (song) && ! doesLoopFollowSong (song);
}

void setPunchOn (Song& song, bool on)
{
    song.getState().setProperty (schema::punch, on, nullptr); // like the loop's switch, not an undo step
}

bool isPunching (const Song& song)
{
    return isPunchOn (song) && canPunch (song);
}

void togglePunch (Song& song)
{
    setPunchOn (song, ! isPunchOn (song));
}

//==============================================================================
namespace
{
    // The recorder of the selection's song, which its session owns, or nullptr.
    AudioRecorder* findRecorder (const Selection& selection)
    {
        auto* song = selection.getSong();
        auto* session = song != nullptr ? findSession (*song) : nullptr;
        return session != nullptr ? &session->getRecorder() : nullptr;
    }

    // The selected tracks that can record audio and have an input, which arming arms.
    std::vector<TrackID> getArmableTracks (const Selection& selection)
    {
        std::vector<TrackID> tracks;

        if (auto* song = selection.getSong(); song != nullptr && findRecorder (selection) != nullptr)
            for (const auto id : selection.getTracks())
                if (canRecordAudio (*song, id) && ! getInputChannels (findTrackState (*song, id)[schema::trackInput].toString()).empty())
                    tracks.push_back (id);

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
    return ! getArmableTracks (selection).empty();
}

bool areSelectedTracksArmed (const Selection& selection)
{
    const auto tracks = getArmableTracks (selection);
    const auto* recorder = findRecorder (selection);
    return recorder != nullptr && areAllOn (tracks, [recorder] (TrackID track) { return recorder->isArmed (track); });
}

void toggleArmSelectedTracks (const Selection& selection)
{
    if (auto* recorder = findRecorder (selection))
        toggleAll (
            getArmableTracks (selection), [recorder] (TrackID track) { return recorder->isArmed (track); },
            [recorder] (TrackID track, bool on) { recorder->setArmed (track, on); });
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
void addRecordingCommands (CommandRegistry& registry, Selection& selection, AudioIO& audio)
{
    registry.add ({ .id = "transport.record",
                    .name = "Record",
                    .category = "Transport",
                    .description = "Records what you play onto the track live MIDI plays, and the armed tracks' audio inputs, from where "
                                   "playback starts, or stops recording.",
                    .synonyms = { "rec", "take", "record midi", "record audio" },
                    .perform = withSong (selection, toggleRecording),
                    .isEnabled = ifSong (selection, canToggleRecording),
                    .isTicked = ifSong (selection, isRecording) });

    registry.add ({ .id = "transport.overdub",
                    .name = "Overdub",
                    .category = "Transport",
                    .description = "Recording adds what you play to the clips already there; off, it takes their place.",
                    .synonyms = { "merge recordings", "replace", "blend", "record mode" },
                    .perform = [&audio] { toggleOverdub (audio); },
                    .isTicked = [&audio] { return isOverdubOn (audio); } });

    registry.add ({ .id = "transport.quantizeRecordings",
                    .name = "Quantize Recordings",
                    .category = "Transport",
                    .description = "Moves the notes you record to the nearest sixteenth as they go in.",
                    .synonyms = { "input quantize", "record quantize", "snap recording", "timing" },
                    .perform = [&audio] { toggleRecordQuantize (audio); },
                    .isTicked = [&audio] { return isRecordQuantizeOn (audio); } });

    registry.add ({ .id = "transport.punch",
                    .name = "Punch In",
                    .category = "Transport",
                    .description = "Record keeps only what is played in the looped range, in place of what was there, cross-fading at its "
                                   "edges. Loop a range to punch in.",
                    .synonyms = { "punch in", "punch out", "auto punch", "replace a part", "drop in" },
                    .perform = withSong (selection, togglePunch),
                    .isEnabled = ifSong (selection, canPunch),
                    .isTicked = ifSong (selection, isPunching) });

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
