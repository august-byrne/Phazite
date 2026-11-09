// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Import.h"
#include "internal/AudioClips.h"
#include "internal/Clips.h"
#include "internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/AudioFiles.h"
#include "model/mixer/internal/Mixer.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace phazite
{
namespace
{
    // Whether a dropped file can go on a track. Audio and notes don't share a track in v1 (docs/design.md,
    // section 5): an instrument would silence the audio.
    bool suitsTrack (const Song& song, TrackID track, FileKind kind)
    {
        const auto state = findTrackState (song, track);

        if (! state.hasType (schema::TRACK) || isFolderState (state))
            return false;

        for (const auto& clipState : getClipStates (state))
            if (isMidiState (clipState) != (kind == FileKind::midi))
                return false;

        return kind == FileKind::midi || ! hasInstrument (song, track);
    }

    // What a MIDI file holds for a clip: the notes and controller events of its first track with notes, as the song keeps
    // them (Schema.h: NOTE, CONTROL), in beats (quarter notes) from the file's start, and its name: the track's own, or
    // else the file's; and the file's tempos, each from its beat, from any of its tracks. Nothing for a file that can't be
    // read or that holds no notes.
    struct MidiFileClip
    {
        juce::String name;
        std::vector<juce::ValueTree> notes, controls;
        double lastBeat = 0.0;
        std::vector<TempoChange> tempos;
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

        // Its tempos, in a file timed in ticks: each held until the next, as MIDI's tempo changes are.
        std::vector<TempoChange> tempos;

        if (ticks > 0.0)
        {
            juce::MidiMessageSequence tempoEvents;
            midi.findAllTempoEvents (tempoEvents);

            for (const auto* event : tempoEvents)
                if (const auto seconds = event->message.getTempoSecondsPerQuarterNote(); seconds > 0.0)
                    tempos.push_back (
                        { BeatPosition::fromBeats (std::max (0.0, toBeats (event->message.getTimeStamp()))), 60.0 / seconds });
        }

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

                clip.tempos = tempos;
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

juce::File copyIntoProject (Song& song, const juce::File& file)
{
    const auto& projectFile = song.getFile();

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

ClipID importFile (Song& song, TrackID track, const juce::File& file, BeatPosition start)
{
    switch (getFileKind (file))
    {
        case FileKind::audio:
        {
            const auto copy = copyIntoProject (song, file);
            const auto info = getAudioFileInfo (copy);

            if (info.getSeconds() <= 0.0)
                return {};

            const auto clipState = makeAudioClipState (song, file.getFileNameWithoutExtension(), copy, start, { 0.0, info.getSeconds() });
            const auto clip = placeClipState (song, clipState, track);

            // A loop that knows its beats (from its loop data, or from its name when it lasts whole bars) follows the song's
            // tempo, as the browser's preview played it: stretched to its beats from where it starts.
            if (clip.isValid() && info.loopBeats > 0.0)
                setClipFollowsTempo (song, clip, true);

            return clip;
        }

        case FileKind::midi:
        {
            // As long as the file's bars from its start to the end of the bar its last event is in, each note with an ID of
            // the song's, where it is dropped.
            const auto content = readMidiFile (file);

            if (! content.has_value())
                return {};

            // A file whose tempo changes brings them into a song that has neither clips nor tempo changes yet, from where it
            // is dropped, so that its notes play at their times; otherwise the song keeps its tempo, as a dropped file's
            // single tempo leaves it.
            const auto& tempos = content->tempos;
            const auto changes = std::any_of (tempos.begin(), tempos.end(), [&tempos] (const TempoChange& tempo)
                                              { return std::abs (tempo.bpm - tempos.front().bpm) >= 0.005; });

            if (changes && getClipStates (song).empty() && ! hasTempoChanges (song))
                takeTempoChanges (song, tempos, start);

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
            return placeClipState (song, clipState, track);
        }

        case FileKind::other:
        default:
            return {};
    }
}

std::vector<ClipID> importFiles (Song& song, const juce::StringArray& paths, TrackID target, BeatPosition start)
{
    // The first file can go on the target track; the others get new tracks below it.
    auto after = target.isValid() ? target : TrackID();

    if (! after.isValid())
        if (const auto tracks = getTrackIDs (song); ! tracks.empty())
            after = tracks.back();

    std::vector<ClipID> added;
    const UndoStep step (song, paths.size() > 1 ? "Add Files" : "Add File");

    for (const auto& path : paths)
    {
        const juce::File file (path);
        const auto kind = getFileKind (file);

        if (kind == FileKind::other)
            continue;

        // A new track is named after the file, and colored by what its name says it is.
        auto track = target;

        if (! track.isValid() || ! suitsTrack (song, track, kind))
        {
            track = insertTrack (song, after, nullptr);

            if (! track.isValid())
                break;

            setTrackName (song, track, file.getFileNameWithoutExtension());

            if (const auto colour = getColourForName (file.getFileNameWithoutExtension()))
                setTrackColour (song, track, *colour);
        }

        if (const auto clip = importFile (song, track, file, start); clip.isValid())
            added.push_back (clip);

        after = track;
        target = {};
    }

    return added;
}
} // namespace phazite
