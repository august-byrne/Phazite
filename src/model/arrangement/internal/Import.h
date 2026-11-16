// SPDX-License-Identifier: AGPL-3.0-or-later
// Bringing files into a project: audio and MIDI files dropped onto the playlist from Explorer or the browser, or
// put there with the browser's Enter. Audio is copied into the project's Audio folder, so the project keeps working
// when the original moves, and Save As takes it along. A loop that knows its tempo follows the song's.
#pragma once

#include "AudioWork.h"
#include "model/base/Beats.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/engine/AudioFiles.h"

#include <juce_graphics/juce_graphics.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace phazite
{
enum class FileKind
{
    audio,
    midi,
    other
};

// What a file is, by its extension: audio that Phazite reads (model/engine/AudioFiles.h), a MIDI file, or neither.
FileKind getFileKind (const juce::File&);

// The project's copy of an audio file, in its Audio folder. The file is copied there unless it is in the
// project already or the folder has an identical copy; a different file of the same name gets a new
// name. Returns the original if the song has no project file (Song::getFile()) or the copy fails.
juce::File copyIntoProject (Song&, const juce::File&);

// A color for a track named after a file, by the words in its name: warm colors for drums, blue for bass,
// and so on (docs/design.md, section 5). Nothing if the name says nothing about the sound.
std::optional<juce::Colour> getColourForName (const juce::String&);

// Adds a file as a clip starting at a beat: an audio clip, from the project's copy of the file (a loop that knows its
// tempo stretched to the song's, as the browser previews it), or a MIDI clip. Returns none for other files, or if the
// file can't be read.
ClipID importFile (Song&, TrackID, const juce::File&, BeatPosition start);

// Adds files dropped on the playlist, at a beat, as one undo step ("Add File" or "Add Files"). The first goes on the target
// track if it suits it (MIDI on a track with no audio clips; audio on one with no MIDI clips and no instrument, which would
// silence it); each other file, and one that doesn't suit the track, goes on a new track below, named after the file and
// colored by what its name says. Files that are neither audio nor MIDI are left out. Returns the clips added. These read the
// files at once, on the calling thread: for what Phazite makes itself (an example song) and tests. What the user drops is
// read in the background (makeImportJob()).
std::vector<ClipID> importFiles (Song&, const juce::StringArray& paths, TrackID target, BeatPosition start);

//==============================================================================
// An import in two halves, so that what reads the files never holds the window (known bug 123): a file as read for it, off
// the song (any thread): what it is, and for audio, the project's copy (copyIntoProject(), for the project file that was
// the song's as the import started) and what that copy holds, and whether the import wrote it; then its clip, from that,
// on the message thread.
struct ImportedFile
{
    juce::File file;
    FileKind kind = FileKind::other;
    juce::File copy;
    AudioFileInfo info;
    bool wroteCopy = false;
    bool brought = false; // copied, or found the same by its content: remembered once the import keeps it (copyIntoProject())
};

// Nothing once shouldStop says so, which it asks between megabytes; progress hears how much of the work is done, from 0 to 1.
std::optional<ImportedFile> readForImport (const juce::File&, const juce::File& projectFile, const std::function<bool()>& shouldStop,
                                           const std::function<void (float done)>& progress = nullptr);

ClipID addImportedFile (Song&, TrackID, const ImportedFile&, BeatPosition start);                                   // in the caller's step
std::vector<ClipID> addImportedFiles (Song&, const std::vector<ImportedFile>&, TrackID target, BeatPosition start); // as importFiles()

// The job (AudioWork.h) of an import of the files the user drops: read in the background, then added as importFiles() adds
// them, as one undo step, and added called with the clips added. Nothing changes, and the user is told why, if the track
// they were dropped on went meanwhile, or the project moved to another folder. Nothing for no file that can be imported.
std::unique_ptr<AudioWork::Job> makeImportJob (const Song&, const juce::StringArray& paths, TrackID target, BeatPosition start,
                                               std::function<void (const std::vector<ClipID>&)> added);

// Files brought into the project for a change that names them (a sound put on a drum pad, the sampler or a wavetable; Swap's
// keep; a missing file found again): one way, as an import copies them (copyIntoProject()), in the background as a job of
// the song's work (AudioWork.h), then the change, on the message thread, which finds each copy made without reading
// anything (copyIntoProject() remembers what it brought in). Files inside the project, or a song without its work (a test's
// alone), need no job: prepare and apply at once.
struct FilesToBring
{
    juce::Array<juce::File> files;
    juce::String title;              // as the progress window says it: "Copying Kick.wav"
    juce::String problem;            // what the user is told when the change doesn't take them
    std::function<void()> prepare;   // on the work's thread, after the copies: what else the change reads (a sampler's root key)
    std::function<bool()> apply;     // the change: false if it couldn't be made (its device went meanwhile): nothing changes
    std::function<void()> otherwise; // instead, if it is cancelled or another project opens (a Swap's trial put back)
    bool forTheOpenTrial = false;    // it finishes the Swap's trial open now
};

// What apply did at once, or true once its job started.
bool bringFilesIntoProject (const Song&, FilesToBring);
} // namespace phazite
