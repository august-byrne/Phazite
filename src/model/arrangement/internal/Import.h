// SPDX-License-Identifier: AGPL-3.0-or-later
// Bringing files into a project: audio and MIDI files dropped onto the playlist from Explorer or the browser, or
// put there with the browser's Enter. Audio is copied into the project's Audio folder, so the project keeps working
// when the original moves, and Save As takes it along. A loop that knows its tempo follows the song's.
#pragma once

#include "model/base/Beats.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"

#include <juce_graphics/juce_graphics.h>

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
// colored by what its name says. Files that are neither audio nor MIDI are left out. Returns the clips added.
std::vector<ClipID> importFiles (Song&, const juce::StringArray& paths, TrackID target, BeatPosition start);
} // namespace phazite
