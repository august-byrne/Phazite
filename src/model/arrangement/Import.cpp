// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Import.h"
#include "internal/AudioClips.h"
#include "internal/Clips.h"
#include "internal/Tracks.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/AudioFiles.h"
#include "model/engine/internal/LiveInput.h"
#include "model/transport/internal/Tempo.h"

#include <optional>
#include <vector>

namespace phazite
{
namespace
{
    // Whether a dropped file can go on a track. Audio and notes don't share a track in v1 (docs/design.md,
    // section 5): an instrument would silence the audio.
    bool suitsTrack (te::AudioTrack& track, FileKind kind)
    {
        for (auto* clip : track.getClips())
            if ((dynamic_cast<te::MidiClip*> (clip) != nullptr) != (kind == FileKind::midi))
                return false;

        return kind == FileKind::midi || ! hasInstrument (track);
    }

    // What a MIDI file holds for a clip: the notes and controller events of its first track with notes, as the song keeps
    // them (Schema.h: NOTE, CONTROL), in beats (quarter notes) from the file's start, and its name: the track's own, or
    // else the file's. Nothing for a file that can't be read or that holds no notes.
    struct MidiFileClip
    {
        juce::String name;
        std::vector<juce::ValueTree> notes, controls;
        double lastBeat = 0.0;
    };

    std::optional<MidiFileClip> readMidiFile (const juce::File& file)
    {
        juce::FileInputStream stream (file);
        juce::MidiFile midi;

        if (! stream.openedOk() || ! midi.readFrom (stream))
            return std::nullopt;

        // So many ticks to a quarter note; a file timed in frames is read in seconds, at MIDI's 120 BPM.
        const auto ticks = (double) midi.getTimeFormat();

        if (ticks <= 0.0)
            midi.convertTimestampTicksToSeconds();

        const auto toBeats = [ticks] (double time) { return ticks > 0.0 ? time / ticks : time * 2.0; };

        for (int index = 0; index < midi.getNumTracks(); ++index)
        {
            juce::MidiMessageSequence sequence (*midi.getTrack (index));
            sequence.updateMatchedPairs();
            MidiFileClip clip;

            for (const auto* event : sequence)
            {
                const auto& message = event->message;
                const auto beat = std::max (0.0, toBeats (message.getTimeStamp()));
                juce::ValueTree control (schema::CONTROL);

                if (message.isTrackNameEvent())
                {
                    clip.name = message.getTextFromTextMetaEvent().trim();
                }
                else if (message.isNoteOn())
                {
                    const auto end = event->noteOffObject != nullptr ? toBeats (event->noteOffObject->message.getTimeStamp()) : beat;
                    juce::ValueTree note (schema::NOTE);
                    note.setProperty (schema::notePitch, message.getNoteNumber(), nullptr);
                    note.setProperty (schema::noteStart, beat, nullptr);
                    note.setProperty (schema::noteLength, std::max (end - beat, 1.0 / 64.0), nullptr);
                    note.setProperty (schema::noteVelocity, (int) message.getVelocity(), nullptr);
                    clip.notes.push_back (note);
                    clip.lastBeat = std::max (clip.lastBeat, end);
                }
                else if (message.isController())
                {
                    control.setProperty (schema::controlKind, schema::controlIsController, nullptr);
                    control.setProperty (schema::controlNumber, message.getControllerNumber(), nullptr);
                    control.setProperty (schema::controlValue, message.getControllerValue(), nullptr);
                }
                else if (message.isPitchWheel())
                {
                    control.setProperty (schema::controlKind, schema::controlIsPitchWheel, nullptr);
                    control.setProperty (schema::controlValue, message.getPitchWheelValue(), nullptr);
                }
                else if (message.isChannelPressure())
                {
                    control.setProperty (schema::controlKind, schema::controlIsChannelPressure, nullptr);
                    control.setProperty (schema::controlValue, message.getChannelPressureValue(), nullptr);
                }
                else if (message.isAftertouch())
                {
                    control.setProperty (schema::controlKind, schema::controlIsAftertouch, nullptr);
                    control.setProperty (schema::controlNumber, message.getNoteNumber(), nullptr);
                    control.setProperty (schema::controlValue, message.getAfterTouchValue(), nullptr);
                }
                else if (message.isProgramChange())
                {
                    control.setProperty (schema::controlKind, schema::controlIsProgramChange, nullptr);
                    control.setProperty (schema::controlValue, message.getProgramChangeNumber(), nullptr);
                }

                if (control.hasProperty (schema::controlKind))
                {
                    control.setProperty (schema::controlBeat, beat, nullptr);
                    clip.lastBeat = std::max (clip.lastBeat, beat);
                    clip.controls.push_back (control);
                }
            }

            if (! clip.notes.empty())
            {
                if (clip.name.isEmpty())
                    clip.name = file.getFileNameWithoutExtension();

                return clip;
            }
        }

        return std::nullopt;
    }
} // namespace

FileKind getFileKind (const juce::File& file)
{
    const auto extension = file.getFileExtension().toLowerCase();

    if (extension == ".mid" || extension == ".midi")
        return FileKind::midi;

    if (extension.isNotEmpty() && getAudioFormats().findFormatForFileExtension (extension) != nullptr)
        return FileKind::audio;

    return FileKind::other;
}

juce::File copyIntoProject (te::Edit& edit, const juce::File& file)
{
    const auto projectFile = edit.editFileRetriever != nullptr ? edit.editFileRetriever() : juce::File();

    if (projectFile == juce::File() || file.isAChildOf (projectFile.getParentDirectory()))
        return file;

    const auto audioFolder = projectFile.getSiblingFile ("Audio");
    auto target = audioFolder.getChildFile (file.getFileName());

    if (target.existsAsFile() && target.hasIdenticalContentTo (file))
        return target; // imported before

    if (target.exists())
        target = audioFolder.getNonexistentChildFile (file.getFileNameWithoutExtension(), file.getFileExtension(), false);

    if (! audioFolder.createDirectory() || ! file.copyFileTo (target))
        return file;

    return target;
}

std::optional<juce::Colour> getColourForName (const juce::String& name)
{
    struct Family
    {
        const char* colourName; // in getTrackColours()
        juce::StringArray words;
    };

    // The first family with a word in the name wins, so the more specific words come first.
    static const Family families[] = {
        { "Blue", { "bass", "808", "sub" } },
        { "Vermilion",
          { "kick", "snare", "clap", "hihat", "hi-hat", "hat", "perc", "drum", "cymbal", "crash", "shaker", "conga", "bongo" } },
        { "Reddish purple", { "vocal", "vox", "voice", "choir", "acapella", "adlib" } },
        { "Yellow", { "fx", "riser", "impact", "sweep", "noise", "uplifter", "downlifter", "whoosh" } },
        { "Bluish green", { "guitar", "gtr", "string", "violin", "cello", "brass", "horn", "flute" } },
        { "Sky blue", { "synth", "lead", "pad", "chord", "keys", "piano", "pluck", "arp", "bell", "organ" } },
    };

    const auto lower = name.toLowerCase();

    for (const auto& family : families)
        for (const auto& word : family.words)
            if (lower.contains (word))
                if (const auto colour = findTrackColour (family.colourName))
                    return colour;

    return std::nullopt;
}

te::Clip* importFile (te::ClipTrack& track, const juce::File& file, te::BeatPosition start)
{
    auto& edit = track.edit;

    switch (getFileKind (file))
    {
        case FileKind::audio:
        {
            const auto copy = copyIntoProject (edit, file);

            // Tracktion's clips, which read the file too while they mirror the song's, keep what Tracktion read of a file by
            // its path until it checks the file again: read again if it changed since (a file written where another was,
            // or where none was yet when Tracktion was asked about it).
            edit.engine.getAudioFileManager().checkFileForChanges (te::AudioFile (edit.engine, copy));
            const auto info = getAudioFileInfo (copy);

            if (info.getSeconds() <= 0.0)
                return nullptr;

            const auto clipState =
                makeAudioClipState (edit, file.getFileNameWithoutExtension(), copy, fromTracktion (start), { 0.0, info.getSeconds() });
            auto* clip = dynamic_cast<te::AudioClipBase*> (placeClipState (edit, clipState, track));

            if (clip == nullptr)
                return nullptr;

            // A loop that knows its beats (from its loop data, or from its name when it lasts whole bars) follows the song's
            // tempo, as the browser's preview played it: stretched to its beats from where it starts.
            if (info.loopBeats > 0.0)
                setClipFollowsTempo (*clip, true);

            return clip;
        }

        case FileKind::midi:
        {
            // As long as the file's bars from its start to the end of the bar its last event is in, each note with an ID of
            // the song's, where it is dropped.
            const auto content = readMidiFile (file);

            if (! content.has_value())
                return nullptr;

            auto& song = getSong (edit);
            const auto bars = getBars (song);
            const auto end = bars.getBarStart (bars.getBarAt (BeatPosition::fromBeats (std::max (0.0, content->lastBeat - 1.0e-6))) + 1);
            auto clipState = makeMidiClipState (song, content->name, { BeatPosition(), end });

            for (auto note : content->notes)
            {
                note.setProperty (schema::noteID, (juce::int64) song.makeID(), nullptr);
                clipState.appendChild (note, nullptr);
            }

            for (const auto& control : content->controls)
                clipState.appendChild (control, nullptr);

            clipState.setProperty (schema::clipStart, std::max (0.0, start.inBeats()), nullptr);
            return placeClipState (edit, clipState, track);
        }

        case FileKind::other:
        default:
            return nullptr;
    }
}

juce::Array<te::Clip*> importFiles (te::Edit& edit, const juce::StringArray& paths, te::Track* target, te::BeatPosition start)
{
    // The first file can go on the target track; the others get new tracks below it.
    auto* after = target != nullptr ? target : getPlaylistTracks (edit).getLast();
    juce::Array<te::Clip*> added;
    const UndoStep step (edit, paths.size() > 1 ? "Add Files" : "Add File");

    for (const auto& path : paths)
    {
        const juce::File file (path);
        const auto kind = getFileKind (file);

        if (kind == FileKind::other)
            continue;

        // A new track is named after the file, and colored by what its name says it is.
        auto* track = dynamic_cast<te::AudioTrack*> (target);

        if (track == nullptr || ! suitsTrack (*track, kind))
        {
            track = insertTrack (edit, after, nullptr);

            if (track == nullptr)
                break;

            setTrackName (*track, file.getFileNameWithoutExtension());

            if (const auto colour = getColourForName (file.getFileNameWithoutExtension()))
                setTrackColour (*track, *colour);
        }

        if (auto* clip = importFile (*track, file, start))
            added.add (clip);

        after = track;
        target = nullptr;
    }

    return added;
}
} // namespace phazite
