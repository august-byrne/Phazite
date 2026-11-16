// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Import.h"
#include "internal/AudioClips.h"
#include "internal/Clips.h"
#include "internal/Tracks.h"
#include "model/base/WholeFile.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/AudioFiles.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/ProjectContents.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

namespace phazite
{
namespace
{
    // Whether a dropped file can go on a track (canTrackHold(): audio and notes don't share a track, and a return or a folder
    // holds no clips; a file dropped on a return went on it, unheard).
    bool suitsTrack (const Song& song, TrackID track, FileKind kind)
    {
        return kind != FileKind::other && canTrackHold (song, track, kind == FileKind::midi);
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

        // Within a song's beats (maxSongBeats), as a project's are kept as it loads: a file from others can time an event
        // a hundred million million beats along, past what a bar's number holds (known bug 148).
        const auto toBeats = [ticks] (double time)
        { return juce::jlimit (-maxSongBeats, maxSongBeats, ticks > 0.0 ? time / ticks : time * 2.0); };

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

namespace
{
    constexpr int pieceBytes = 1 << 20; // what an import reads between asking whether it should stop

    // Whether two files hold the same bytes, read a piece at a time; nothing once shouldStop says so. A file that can't be
    // read whole holds nothing the same.
    std::optional<bool> isSameContent (const juce::File& a, const juce::File& b, const std::function<bool()>& shouldStop,
                                       const std::function<void (float)>& progress)
    {
        const auto size = a.getSize();

        if (size != b.getSize())
            return false;

        juce::FileInputStream first (a), second (b);

        if (! first.openedOk() || ! second.openedOk())
            return false;

        juce::HeapBlock<char> one (pieceBytes), other (pieceBytes);
        juce::int64 compared = 0;

        for (;;)
        {
            if (shouldStop != nullptr && shouldStop())
                return std::nullopt;

            const auto read = first.read (one, pieceBytes);

            if (read < 0 || second.read (other, read) != read)
                return false;

            AudioWork::noteRead (2 * (juce::int64) read);

            if (read == 0)
                return compared == size && second.isExhausted();

            if (std::memcmp (one, other, (size_t) read) != 0)
                return false;

            compared += read;

            if (progress != nullptr && size > 0)
                progress ((float) ((double) compared / (double) size));
        }
    }

    // A file copied whole, a piece at a time, or not at all: nothing is left where it would go if it can't be read whole,
    // changed while it was read, or shouldStop said so (then stopped is set).
    bool copyWhole (const juce::File& from, const juce::File& to, const std::function<bool()>& shouldStop,
                    const std::function<void (float)>& progress, bool& stopped)
    {
        const auto size = from.getSize();
        juce::int64 copied = 0;

        const auto written = writeWholeFile (to,
                                             [&] (juce::OutputStream& out)
                                             {
                                                 juce::FileInputStream in (from);

                                                 if (! in.openedOk())
                                                     return false;

                                                 juce::HeapBlock<char> piece (pieceBytes);

                                                 for (;;)
                                                 {
                                                     if (shouldStop != nullptr && shouldStop())
                                                     {
                                                         stopped = true;
                                                         return false;
                                                     }

                                                     const auto read = in.read (piece, pieceBytes);

                                                     if (read < 0)
                                                         return false;

                                                     // Whole, and into the name kept for it, which nothing else wrote meanwhile.
                                                     if (read == 0)
                                                         return copied == size && from.getSize() == size && to.getSize() == 0;

                                                     AudioWork::noteRead (read);

                                                     if (! out.write (piece, (size_t) read))
                                                         return false;

                                                     copied += read;

                                                     if (progress != nullptr && size > 0)
                                                         progress ((float) ((double) copied / (double) size));
                                                 }
                                             });
        return written.wasOk();
    }

    // The project's copy of an audio file (copyIntoProject()), and whether this wrote it; nothing once shouldStop says so.
    struct Copy
    {
        juce::File file;
        bool written = false;
        bool brought = false; // copied or found the same by its content: to remember (noteBrought()) once it is kept
    };

    // The files brought into a project lately, each with its copy, as the file was then: what a change made once its copy is
    // made (bringFilesIntoProject()) finds without reading either again. And what guards it and the names reserved.
    struct Brought
    {
        juce::String source;
        juce::int64 size = 0, modified = 0;
        juce::File copy;
        juce::int64 copyModified = 0; // a copy written again since, with other audio of the same size, isn't this one
    };

    // These three never go: a read given up and stuck in the system as Phazite quits may return as the process ends, and
    // uses them from its thread then (AudioWork.h: LeftBehind).
    std::mutex& getCopyLock()
    {
        static auto& lock = *new std::mutex();
        return lock;
    }

    std::vector<Brought>& getBrought()
    {
        static auto& brought = *new std::vector<Brought>();
        return brought;
    }

    constexpr size_t mostBrought = 256;

    // The names reserved for copies being written, or written by a job that hasn't been kept or deleted yet (one given up
    // may still be writing, or deleting what it wrote): no other copy compares itself with them or takes them as its own.
    std::set<juce::String>& getReservedNames()
    {
        static auto& names = *new std::set<juce::String>();
        return names;
    }

    bool isReserved (const juce::File& file)
    {
        const std::scoped_lock held (getCopyLock());
        return getReservedNames().count (file.getFullPathName()) > 0;
    }

    void releaseName (const juce::File& file)
    {
        const std::scoped_lock held (getCopyLock());
        getReservedNames().erase (file.getFullPathName());
    }

    std::optional<juce::File> findBrought (const juce::File& file, const juce::File& audioFolder)
    {
        const std::scoped_lock held (getCopyLock());
        const auto size = file.getSize();
        const auto modified = file.getLastModificationTime().toMilliseconds();

        for (const auto& each : getBrought())
            if (each.source == file.getFullPathName() && each.size == size && each.modified == modified
                && getReservedNames().count (each.copy.getFullPathName()) == 0 && each.copy.getParentDirectory() == audioFolder
                && each.copy.getSize() == size && each.copy.getLastModificationTime().toMilliseconds() == each.copyModified)
                return each.copy;

        return std::nullopt;
    }

    void noteBrought (const juce::File& file, const juce::File& copy)
    {
        const std::scoped_lock held (getCopyLock());
        auto& brought = getBrought();
        std::erase_if (brought, [&] (const Brought& each) { return each.source == file.getFullPathName() || each.copy == copy; });
        brought.push_back ({ file.getFullPathName(), file.getSize(), file.getLastModificationTime().toMilliseconds(), copy,
                             copy.getLastModificationTime().toMilliseconds() });

        if (brought.size() > mostBrought)
            brought.erase (brought.begin());
    }

    // A name of the Audio folder for a copy, kept for it as an empty file from the moment it is chosen, so that no other copy
    // (another import, a sound put on a pad meanwhile) takes it before the copy is in.
    juce::File reserveName (const juce::File& audioFolder, const juce::File& file)
    {
        const std::scoped_lock held (getCopyLock());

        if (! audioFolder.createDirectory())
            return {};

        auto target = audioFolder.getChildFile (file.getFileName());

        if (target.exists())
            target = audioFolder.getNonexistentChildFile (file.getFileNameWithoutExtension(), file.getFileExtension(), false);

        if (! target.create().wasOk())
            return {};

        getReservedNames().insert (target.getFullPathName());
        return target;
    }

    std::optional<Copy> findOrCopy (const juce::File& file, const juce::File& projectFile, const std::function<bool()>& shouldStop,
                                    const std::function<void (float)>& progress)
    {
        if (projectFile == juce::File() || file.isAChildOf (projectFile.getParentDirectory()))
            return Copy { file };

        const auto audioFolder = projectFile.getSiblingFile ("Audio");

        if (const auto known = findBrought (file, audioFolder))
            return Copy { *known };

        // Imported before: the comparison is half the work, the copy (if it differs) the other half.
        const auto half = [&progress] (float offset)
        {
            return [&progress, offset] (float done)
            {
                if (progress != nullptr)
                    progress (offset + 0.5f * done);
            };
        };

        if (const auto same = audioFolder.getChildFile (file.getFileName());
            same.existsAsFile() && same.getSize() > 0 && ! isReserved (same))
        {
            const auto identical = isSameContent (same, file, shouldStop, half (0.0f));

            if (! identical.has_value())
                return std::nullopt;

            if (*identical)
                return Copy { same, false, true };
        }

        if (shouldStop != nullptr && shouldStop())
            return std::nullopt;

        const auto target = reserveName (audioFolder, file);

        if (target == juce::File())
            return Copy { file };

        auto stopped = false;

        if (! copyWhole (file, target, shouldStop, half (0.5f), stopped))
        {
            if (target.getSize() == 0) // still the name kept for it alone
                target.deleteFile();

            releaseName (target);

            return stopped ? std::nullopt : std::optional<Copy> (Copy { file });
        }

        // Given up as its last piece went in (its job abandoned): it is no one's.
        if (shouldStop != nullptr && shouldStop())
        {
            target.deleteFile();
            releaseName (target);
            return std::nullopt;
        }

        return Copy { target, true, true };
    }

    // The copies a job wrote, as its read's thread adds them, which the message thread takes once the job ends: deleted at
    // once, as they come, once the job has been given up (abandon()), when no one will take them.
    class CopiesMade
    {
    public:
        // On the read's thread: false (and the copy deleted) once given up.
        bool add (const juce::File& copy)
        {
            const std::scoped_lock held (lock);

            if (givenUp)
            {
                copy.deleteFile();
                releaseName (copy);
                return false;
            }

            files.add (copy);
            return true;
        }

        // Message thread: those it wrote, kept (or about to be deleted): their names free from now on.
        juce::Array<juce::File> take()
        {
            const std::scoped_lock held (lock);
            auto taken = std::exchange (files, {});

            for (const auto& file : taken)
                releaseName (file);

            return taken;
        }

        // Message thread: those made so far, and the job given up from now on.
        juce::Array<juce::File> giveUp()
        {
            const std::scoped_lock held (lock);
            givenUp = true;
            auto taken = std::exchange (files, {});

            for (const auto& file : taken)
                releaseName (file);

            return taken;
        }

    private:
        std::mutex lock;
        juce::Array<juce::File> files;
        bool givenUp = false;
    };
} // namespace

juce::File copyIntoProject (Song& song, const juce::File& file)
{
    const auto copy = findOrCopy (file, song.getFile(), nullptr, nullptr).value_or (Copy { file });

    if (copy.written)
        releaseName (copy.file); // kept

    if (copy.brought)
        noteBrought (file, copy.file);

    return copy.file;
}

std::optional<ImportedFile> readForImport (const juce::File& file, const juce::File& projectFile, const std::function<bool()>& shouldStop,
                                           const std::function<void (float)>& progress)
{
    ImportedFile read { file, getFileKind (file) };

    if (read.kind != FileKind::audio)
        return read;

    const auto copy = findOrCopy (file, projectFile, shouldStop, progress);

    if (! copy.has_value())
        return std::nullopt;

    read.copy = copy->file;
    read.wroteCopy = copy->written;
    read.brought = copy->brought;

    // What it holds, from its own reader: getAudioFileInfo() keeps what it read for the message thread alone.
    if (const std::unique_ptr<juce::AudioFormatReader> reader (getAudioFormats().createReaderFor (read.copy)); reader != nullptr)
        read.info = readAudioFileInfo (*reader, read.copy.getFileNameWithoutExtension());

    return read;
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

namespace
{
    // A copy an import made or found, remembered once it is kept, for the changes that bring the same file in after it.
    void keepBrought (const ImportedFile& imported)
    {
        if (imported.wroteCopy)
            releaseName (imported.copy);

        if (imported.brought)
            noteBrought (imported.file, imported.copy);
    }
} // namespace

ClipID importFile (Song& song, TrackID track, const juce::File& file, BeatPosition start)
{
    const auto read = readForImport (file, song.getFile(), nullptr);

    if (read.has_value())
        keepBrought (*read);
    return read.has_value() ? addImportedFile (song, track, *read, start) : ClipID();
}

ClipID addImportedFile (Song& song, TrackID track, const ImportedFile& imported, BeatPosition start)
{
    const auto& file = imported.file;

    switch (imported.kind)
    {
        case FileKind::audio:
        {
            const auto& copy = imported.copy;
            const auto& info = imported.info;

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
    std::vector<ImportedFile> read;

    for (const auto& path : paths)
        if (auto file = readForImport (juce::File (path), song.getFile(), nullptr))
        {
            keepBrought (*file);
            read.push_back (std::move (*file));
        }

    return addImportedFiles (song, read, target, start);
}

std::vector<ClipID> addImportedFiles (Song& song, const std::vector<ImportedFile>& files, TrackID target, BeatPosition start)
{
    // The first file can go on the target track; the others get new tracks below it.
    auto after = target.isValid() ? target : TrackID();

    if (! after.isValid())
        if (const auto tracks = getTrackIDs (song); ! tracks.empty())
            after = tracks.back();

    std::vector<ClipID> added;
    const UndoStep step (song, files.size() > 1 ? "Add Files" : "Add File");

    for (const auto& imported : files)
    {
        const auto& file = imported.file;
        const auto kind = imported.kind;

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

        if (const auto clip = addImportedFile (song, track, imported, start); clip.isValid())
            added.push_back (clip);

        after = track;
        target = {};
    }

    return added;
}

//==============================================================================
namespace
{
    // Deletes the copies a job wrote that nothing uses, never one the song names (another change may have taken it as its
    // own: a sound copied the same, adopted).
    void deleteUnused (const ProjectContext& context, const juce::Array<juce::File>& written)
    {
        if (written.isEmpty())
            return;

        juce::Array<juce::File> named;

        for (const auto& used : getProjectAudioFiles (context))
            named.add (used.file);

        for (const auto& file : written)
            if (! named.contains (file))
                file.deleteFile();
    }

    // An import of what the user drops (makeImportJob()): what it took of the song as it started, and what it read.
    class ImportJob final : public AudioWork::Job
    {
    public:
        ImportJob (juce::StringArray importable, juce::File project, TrackID onto, BeatPosition at,
                   std::function<void (const std::vector<ClipID>&)> then)
            : paths (std::move (importable)),
              projectFile (std::move (project)),
              target (onto),
              start (at),
              added (std::move (then))
        {
        }

        juce::String getTitle() const override
        {
            return "Importing " + (paths.size() == 1 ? juce::File (paths[0]).getFileName() : juce::String (paths.size()) + " files");
        }

        juce::File getFolder() const override { return projectFile.getParentDirectory(); }

        void read (const BackgroundJobs::ShouldStop& shouldStop, std::atomic<float>& progress) override
        {
            for (int i = 0; i < paths.size(); ++i)
            {
                const auto share = [&progress, i, count = paths.size()] (float done)
                { progress = ((float) i + juce::jlimit (0.0f, 1.0f, done)) / (float) count; };

                auto file = readForImport (juce::File (paths[i]), projectFile, shouldStop, share);

                if (! file.has_value())
                    return; // stopped: what it wrote goes with discard() or abandon()

                if (file->wroteCopy && ! copies.add (file->copy))
                    return; // given up: the copy is deleted

                files.push_back (std::move (*file));
            }
        }

        void finish (ProjectContext& context) override
        {
            auto& song = *findSong (context);

            // Copied into another folder's Audio: the project moved meanwhile (a first save), and its clips would play from
            // the folder it left.
            const auto why = song.getFile() != projectFile ? juce::String ("The project moved to another folder while Phazite copied ")
                                                                 + describeFiles() + " into it."
                             : target.isValid() && ! findTrackState (song, target).isValid()
                                 ? "The track " + describeFiles() + (paths.size() == 1 ? " was" : " were") + " dropped on went before "
                                       + (paths.size() == 1 ? "it was" : "they were") + " read."
                                 : juce::String();

            if (why.isNotEmpty())
            {
                discard (context);
                reportProblem (context, "Nothing was imported", why + " Drop " + (paths.size() == 1 ? "it" : "them") + " again.");
                return;
            }

            copies.take();

            for (const auto& file : files)
                keepBrought (file);

            const auto clips = addImportedFiles (song, files, target, start);

            if (added != nullptr)
                added (clips);
        }

        void discard (ProjectContext& context) override { deleteUnused (context, copies.take()); }

        void abandon (ProjectContext& context) override
        {
            added = nullptr; // which holds the playlist: never let go of on the read's thread
            deleteUnused (context, copies.giveUp());
        }

    private:
        const juce::StringArray paths;
        const juce::File projectFile; // the song's, as it started
        const TrackID target;
        const BeatPosition start;
        std::function<void (const std::vector<ClipID>&)> added;
        std::vector<ImportedFile> files; // as read, on the work's thread, then on the message thread
        CopiesMade copies;               // the copies it wrote, as its read's thread writes them

        juce::String describeFiles() const { return paths.size() == 1 ? juce::File (paths[0]).getFileName() : "the files"; }
    };

    // Files brought into the project for a change that names them (bringFilesIntoProject()).
    class BringJob final : public AudioWork::Job
    {
    public:
        BringJob (FilesToBring what, juce::File project)
            : bring (std::move (what)),
              projectFile (std::move (project))
        {
        }

        juce::String getTitle() const override { return bring.title; }
        bool isForTheOpenTrial() const override { return bring.forTheOpenTrial; }
        juce::File getFolder() const override { return projectFile.getParentDirectory(); }

        void read (const BackgroundJobs::ShouldStop& shouldStop, std::atomic<float>& progress) override
        {
            for (int i = 0; i < bring.files.size(); ++i)
            {
                const auto share = [&progress, i, count = bring.files.size()] (float fraction)
                { progress = ((float) i + juce::jlimit (0.0f, 1.0f, fraction)) / (float) count; };

                const auto copy = findOrCopy (bring.files[i], projectFile, shouldStop, share);

                if (! copy.has_value())
                    return;

                if (copy->written && ! copies.add (copy->file))
                    return; // given up: the copy is deleted

                if (copy->brought)
                    brought.emplace_back (bring.files[i], copy->file);
            }

            if (shouldStop())
                return;

            if (bring.prepare != nullptr)
                bring.prepare();

            done = true;
        }

        void finish (ProjectContext& context) override
        {
            // Into the project it started in: a copy made into another folder's Audio would be named from there. What it
            // brought in is remembered first, so that the change finds each copy without reading again.
            if (done && findSong (context)->getFile() == projectFile)
            {
                const auto made = copies.take();

                for (const auto& [file, copy] : brought)
                    noteBrought (file, copy);

                if (bring.apply())
                {
                    bring.otherwise = nullptr;
                    return;
                }

                deleteUnused (context, made); // its change not made (what it was for went): what it wrote goes
            }

            discard (context);
            reportProblem (context, "Nothing was changed", bring.problem);
        }

        void discard (ProjectContext& context) override
        {
            deleteUnused (context, copies.take());

            if (const auto instead = std::exchange (bring.otherwise, nullptr))
                instead();
        }

        void abandon (ProjectContext& context) override
        {
            deleteUnused (context, copies.giveUp());

            if (const auto instead = std::exchange (bring.otherwise, nullptr))
                instead();

            bring.apply = nullptr; // which may hold a Swap's trial: never let go of on the read's thread
        }

    private:
        FilesToBring bring;
        const juce::File projectFile;
        CopiesMade copies;
        std::vector<std::pair<juce::File, juce::File>> brought; // each file with its copy, as read
        bool done = false;
    };
} // namespace

std::unique_ptr<AudioWork::Job> makeImportJob (const Song& song, const juce::StringArray& paths, TrackID target, BeatPosition start,
                                               std::function<void (const std::vector<ClipID>&)> added)
{
    juce::StringArray importable;

    for (const auto& path : paths)
        if (getFileKind (juce::File (path)) != FileKind::other)
            importable.add (path);

    if (importable.isEmpty())
        return nullptr;

    return std::make_unique<ImportJob> (importable, song.getFile(), target, start, std::move (added));
}

bool bringFilesIntoProject (const Song& song, FilesToBring bring)
{
    // Those inside the project already are its own (copyIntoProject() reads nothing for them): the change at once.
    const auto& projectFile = song.getFile();
    auto* work = AudioWork::find (song);
    const auto needsCopy = std::any_of (bring.files.begin(), bring.files.end(), [&projectFile] (const juce::File& file)
                                        { return projectFile != juce::File() && ! file.isAChildOf (projectFile.getParentDirectory()); });

    if (! needsCopy || work == nullptr)
    {
        if (bring.prepare != nullptr)
            bring.prepare();

        return bring.apply();
    }

    work->add (std::make_unique<BringJob> (std::move (bring), projectFile));
    return true;
}
} // namespace phazite
