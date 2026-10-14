// SPDX-License-Identifier: AGPL-3.0-or-later
// Bringing files into a project: audio and MIDI files dropped onto the playlist from Explorer or the browser, or
// put there with the browser's Enter. Audio is copied into the project's Audio folder, so the project keeps working
// when the original moves, and Save As takes it along. A loop that knows its tempo follows the song's.
#pragma once

#include "model/base/Tracktion.h"

#include <optional>

namespace phazite
{
enum class FileKind
{
    audio,
    midi,
    other
};

// What a file is, by its extension: audio that Tracktion can read, a MIDI file, or neither.
FileKind getFileKind (te::Engine&, const juce::File&);

// The project's copy of an audio file, in its Audio folder. The file is copied there unless it is in the
// project already or the folder has an identical copy; a different file of the same name gets a new
// name. Returns the original if the Edit has no project file or the copy fails.
juce::File copyIntoProject (te::Edit&, const juce::File&);

// A color for a track named after a file, by the words in its name: warm colors for drums, blue for bass,
// and so on (docs/design.md, section 5). Nothing if the name says nothing about the sound.
std::optional<juce::Colour> getColourForName (const juce::String&);

// Adds a file as a clip starting at a beat: an audio clip, from the project's copy of the file (a loop that knows its
// tempo stretched to the song's, as the browser previews it), or a MIDI clip. Returns nullptr for other files, or if
// the file can't be read.
te::Clip* importFile (te::ClipTrack&, const juce::File&, te::BeatPosition start);
} // namespace phazite
