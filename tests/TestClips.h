// SPDX-License-Identifier: AGPL-3.0-or-later
// MIDI clips, audio clips and notes made through the model, by the song and IDs, for tests that build a song without
// TestSong.h: the song holds its clips and notes itself.
#pragma once

#include "TestHelpers.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/notes/internal/Notes.h"
#include "model/transport/internal/Tempo.h"

#include <cmath>

namespace phazite::test
{
// A MIDI clip on a track from a beat to another, with a name. Returns its ID, or none for a track that can't hold it.
inline ClipID addMidiClip (Song& song, TrackID track, BeatRange beats, const juce::String& name = "Notes")
{
    auto clip = makeMidiClip (song, name, beats);
    clip.track = track;
    return addClip (song, clip);
}

// An audio clip on a track playing a file from its start (or from an offset into it), from a time in the song, in its
// seconds, for as long as given.
inline ClipID addAudioClip (Song& song, TrackID track, const juce::String& name, const juce::File& file, double startSeconds,
                            double lengthSeconds, double offsetSeconds = 0.0)
{
    auto clip = makeAudioClip (song, name, file, toBeats (song, startSeconds), { offsetSeconds, offsetSeconds + lengthSeconds });
    clip.track = track;
    return addClip (song, clip);
}

// A note at a beat counted from the clip's content start (where a clip's notes count from, Clips.h: getContentStart()).
inline NoteRef addMidiNote (Song& song, ClipID clip, int pitch, BeatPosition beat, BeatDuration length, int velocity = 100)
{
    return addNote (song, clip, pitch, getContentStart (song, clip) + (beat - BeatPosition()), length, velocity);
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

// Where a clip's notes count from: its start, less how far into its notes it starts (Clips.h: getClipStartOffset()).
inline BeatPosition getContentStartOf (const Song& song, ClipID clip)
{
    const auto start = getClipRange (song, clip).getStart();
    return toBeats (song, toSeconds (song, start) - getClipStartOffset (song, clip));
}

// Beats as text, to a millionth, with no sign on nothing.
inline juce::String describeBeats (double beats)
{
    return juce::String (std::round (beats * 1.0e6) / 1.0e6 + 0.0, 6);
}

// A clip's notes in the order getClipNotes() gives them, each with all it is: its ID, pitch, start counted from the clip's
// content start, length, velocity, channel and flags. Two clips that hold the same notes (aliases) describe them the same.
inline juce::String describeClipNotes (const Song& song, ClipID clip)
{
    const auto from = getContentStartOf (song, clip);
    juce::StringArray notes;

    for (const auto& note : getClipNotes (song, clip))
        notes.add (juce::String ((juce::int64) note.id.getRaw()) + ": " + juce::String (note.pitch) + " at "
                   + describeBeats ((note.start - from).inBeats()) + " for " + describeBeats (note.length.inBeats()) + " v"
                   + juce::String (note.velocity) + " ch" + juce::String (note.channel) + (note.muted ? " muted" : "")
                   + (note.slide ? " slide" : "") + (note.portamento ? " portamento" : ""));

    return notes.joinIntoString ("; ");
}
} // namespace phazite::test
