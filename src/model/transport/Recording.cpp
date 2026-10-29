// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Recording.h"
#include "internal/AudioRecorder.h"
#include "internal/Playback.h"
#include "model/base/Toggles.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/LiveInput.h"

namespace phazite
{
namespace
{
    // Tracktion's names for quantizing to sixteenths (a quarter of a beat in 4/4), and for not quantizing.
    constexpr const char* sixteenths = "1/4 beat";
    constexpr const char* notQuantized = "(none)";

    // Tracktion keeps the MIDI input's settings with its own, for every project.
    void saveSettings (te::MidiInputDevice& device)
    {
        if (auto* merged = dynamic_cast<te::VirtualMidiInputDevice*> (&device))
            merged->saveProps();
    }
} // namespace

bool canRecord (te::Edit& edit)
{
    const auto* recorder = AudioRecorder::findFor (edit);
    return (getLiveMidiTrack (edit) != nullptr && findLiveMidiInput (edit) != nullptr) || (recorder != nullptr && recorder->canRecord());
}

bool isRecording (const te::Edit& edit)
{
    return edit.getTransport().isRecording();
}

void startRecording (te::Edit& edit)
{
    if (isRecording (edit) || ! canRecord (edit))
        return;

    auto& transport = edit.getTransport();

    // Punched in, the take doesn't loop (Tracktion records in one or the other): the loop goes off for it, and stopping
    // puts it back on (Playback.h). The song's own loop doesn't loop a recording either, which runs on past the song's
    // end: for the take, the loop runs from where recording starts to the end of the time line, and stopping puts the
    // song's loop back. A range looped on purpose does loop it.
    if (isPunching (edit))
    {
        edit.recordingPunchInOut = true;
        transport.looping = false;
    }
    else if (isLoopOn (edit) && doesLoopFollowSong (edit))
    {
        transport.setLoopRange ({ transport.isPlaying() ? transport.getPosition() : getPlayStart (edit), te::Edit::getMaximumEditEnd() });
    }

    // With no input of Tracktion's armed: audio is Phazite's to record (AudioRecorder.h), and Tracktion records what is
    // played on MIDI.
    transport.record (false, true);
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
    const auto device = getAllMidiInputs (engine);
    return device == nullptr || ! device->replaceExistingClips;
}

void setOverdubOn (te::Engine& engine, bool on)
{
    if (const auto device = getAllMidiInputs (engine))
    {
        device->mergeRecordings = on;
        device->replaceExistingClips = ! on;
        saveSettings (*device);
    }
}

void toggleOverdub (te::Engine& engine)
{
    setOverdubOn (engine, ! isOverdubOn (engine));
}

bool isRecordQuantizeOn (te::Engine& engine)
{
    const auto device = getAllMidiInputs (engine);
    return device != nullptr && device->quantisation.getType (false) == sixteenths;
}

void setRecordQuantizeOn (te::Engine& engine, bool on)
{
    if (const auto device = getAllMidiInputs (engine))
    {
        device->quantisation.setType (on ? sixteenths : notQuantized);
        saveSettings (*device);
    }
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
    getPhaziteState (edit).setProperty (schema::punch, on, nullptr); // like the loop's switch, not an undo step
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
    juce::Array<te::AudioTrack*> getArmableTracks (const te::SelectionManager& selection)
    {
        juce::Array<te::AudioTrack*> tracks;
        auto* recorder = selection.getEdit() != nullptr ? AudioRecorder::findFor (*selection.getEdit()) : nullptr;

        if (recorder != nullptr)
            for (auto* track : selection.getItemsOfType<te::AudioTrack>())
                if (! getInputChannels (track->state[schema::phaziteInput].toString()).empty() && ! hasInstrument (*track))
                    tracks.add (track);

        return tracks;
    }

    // The selected audio clips with takes.
    juce::Array<te::AudioClipBase*> getClipsWithTakes (const te::SelectionManager& selection)
    {
        juce::Array<te::AudioClipBase*> clips;

        for (auto* clip : selection.getItemsOfType<te::AudioClipBase>())
            if ((int) clip->state.getProperty (schema::phaziteTakes, 0) > 1)
                clips.add (clip);

        return clips;
    }
} // namespace

bool canArmSelectedTracks (const te::SelectionManager& selection)
{
    return ! getArmableTracks (selection).isEmpty();
}

bool areSelectedTracksArmed (const te::SelectionManager& selection)
{
    const auto tracks = getArmableTracks (selection);
    auto* recorder = selection.getEdit() != nullptr ? AudioRecorder::findFor (*selection.getEdit()) : nullptr;
    return recorder != nullptr && areAllOn (tracks, [recorder] (te::AudioTrack* track) { return recorder->isArmed (*track); });
}

void toggleArmSelectedTracks (const te::SelectionManager& selection)
{
    if (auto* recorder = selection.getEdit() != nullptr ? AudioRecorder::findFor (*selection.getEdit()) : nullptr)
        toggleAll (
            getArmableTracks (selection), [recorder] (te::AudioTrack* track) { return recorder->isArmed (*track); },
            [recorder] (te::AudioTrack* track, bool on) { recorder->setArmed (*track, on); });
}

bool canStepTakes (const te::SelectionManager& selection)
{
    return ! getClipsWithTakes (selection).isEmpty();
}

void stepTakes (const te::SelectionManager& selection, int direction)
{
    // The takes lie one after another in the clip's file, each as long as the range they were recorded round.
    for (auto* clip : getClipsWithTakes (selection))
    {
        const auto count = (int) clip->state[schema::phaziteTakes];
        const auto playing = (int) clip->state[schema::phaziteTake];
        const auto next = juce::jlimit (1, count, playing + direction);

        if (next == playing)
            continue;

        const auto length = te::TimeDuration::fromSeconds ((double) clip->state[schema::phaziteTakeLength]);
        clip->setOffset (clip->getPosition().getOffset() + length * (double) (next - playing));
        clip->state.setProperty (schema::phaziteTake, next, &clip->edit.getUndoManager());
    }
}

//==============================================================================
void addRecordingCommands (CommandRegistry& registry, te::SelectionManager& selection)
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
