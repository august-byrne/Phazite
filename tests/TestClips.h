// SPDX-License-Identifier: AGPL-3.0-or-later
// MIDI clips and notes made through the model, for tests that build a song without TestSong.h: the song holds its MIDI
// clips itself, which Tracktion's mirror (docs/tracktion-migration.md, T1 step 3), so that a clip made on Tracktion's side
// alone would be removed as the song's are placed again.
#pragma once

#include "TestHelpers.h"
#include "model/arrangement/internal/Clips.h"
#include "model/notes/internal/Notes.h"

namespace phazite::test
{
// A MIDI clip on a track from a beat to another, with a name.
inline te::MidiClip* addMidiClip (te::ClipTrack& track, te::BeatRange beats, const juce::String& name = "Notes")
{
    auto& edit = track.edit;
    return placeClipState (edit, makeMidiClipState (edit, name, fromTracktion (beats)), track);
}

// A note at a beat counted from the clip's content start, as Tracktion's MidiList::addNote() takes one.
inline te::MidiNote* addMidiNote (te::MidiClip& clip, int pitch, te::BeatPosition beat, te::BeatDuration length, int velocity = 100)
{
    return addNote (clip, pitch, clip.getContentStartBeat() + te::toDuration (beat), length, velocity);
}
} // namespace phazite::test
