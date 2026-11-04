// SPDX-License-Identifier: AGPL-3.0-or-later
// MIDI clips, audio clips and notes made through the model, by the song and IDs, for tests that build a song without
// TestSong.h: the song holds its clips and notes itself.
#pragma once

#include "TestHelpers.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/notes/internal/Notes.h"
#include "model/transport/internal/Tempo.h"

namespace phazite::test
{
// A MIDI clip on a track from a beat to another, with a name. Returns its ID, or none for a track that can't hold it.
inline ClipID addMidiClip (Song& song, TrackID track, BeatRange beats, const juce::String& name = "Notes")
{
    return placeClipState (song, makeMidiClipState (song, name, beats), track);
}

// An audio clip on a track playing a file from its start (or from an offset into it), from a time in the song, in its
// seconds, for as long as given.
inline ClipID addAudioClip (Song& song, TrackID track, const juce::String& name, const juce::File& file, double startSeconds,
                            double lengthSeconds, double offsetSeconds = 0.0)
{
    const auto clipState =
        makeAudioClipState (song, name, file, toBeats (song, startSeconds), { offsetSeconds, offsetSeconds + lengthSeconds });
    return placeClipState (song, clipState, track);
}

// A note at a beat counted from the clip's content start (where a clip's notes count from, Clips.h: getContentStart()).
inline NoteRef addMidiNote (Song& song, ClipID clip, int pitch, BeatPosition beat, BeatDuration length, int velocity = 100)
{
    return addNote (song, clip, pitch, getContentStart (findClipState (song, clip)) + (beat - BeatPosition()), length, velocity);
}

// The beats as Phazite's positions and lengths, for brevity.
inline BeatPosition at (double beats)
{
    return BeatPosition::fromBeats (beats);
}

inline BeatDuration lasting (double beats)
{
    return BeatDuration::fromBeats (beats);
}
} // namespace phazite::test
