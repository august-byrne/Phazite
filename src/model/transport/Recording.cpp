// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Recording.h"
#include "internal/Playback.h"
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
    return getLiveMidiTrack (edit) != nullptr && findLiveMidiInput (edit) != nullptr;
}

bool isRecording (const te::Edit& edit)
{
    return edit.getTransport().isRecording();
}

void startRecording (te::Edit& edit)
{
    auto* track = getLiveMidiTrack (edit);
    auto* input = findLiveMidiInput (edit);

    if (track == nullptr || input == nullptr || isRecording (edit))
        return;

    // The song's own loop doesn't loop a recording, which runs on past the song's end: for the take, the loop runs from
    // where recording starts to the end of the time line, and stopping puts the song's loop back (Playback.h). A range
    // looped on purpose does loop it.
    auto& transport = edit.getTransport();

    if (isLoopOn (edit) && doesLoopFollowSong (edit))
        transport.setLoopRange ({ transport.isPlaying() ? transport.getPosition() : getPlayStart (edit), te::Edit::getMaximumEditEnd() });

    transport.record (false);
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
void addRecordingCommands (CommandRegistry& registry, te::SelectionManager& selection)
{
    registry.add ({ .id = "transport.record",
                    .name = "Record",
                    .category = "Transport",
                    .description = "Records what you play onto the track live MIDI plays, from where playback starts, or stops recording.",
                    .synonyms = { "rec", "take", "record midi", "arm" },
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
}
} // namespace phazite
