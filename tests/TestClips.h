// SPDX-License-Identifier: AGPL-3.0-or-later
// MIDI clips and notes made through the model, for tests that build a song without TestSong.h: the song holds its MIDI
// clips itself, which Tracktion's mirror (docs/tracktion-migration.md, T1 step 3), so that a clip made on Tracktion's side
// alone would be removed as the song's are placed again.
#pragma once

#include "TestHelpers.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/notes/internal/Notes.h"

namespace phazite::test
{
// A MIDI clip on a track from a beat to another, with a name.
inline te::MidiClip* addMidiClip (te::ClipTrack& track, te::BeatRange beats, const juce::String& name = "Notes")
{
    auto& edit = track.edit;
    return dynamic_cast<te::MidiClip*> (placeClipState (edit, makeMidiClipState (getSong (edit), name, fromTracktion (beats)), track));
}

// An audio clip on a track playing a file from its start (or from an offset into it), from one time in the song to another,
// as Tracktion's insertWaveClip() places one.
inline te::WaveAudioClip* addAudioClip (te::ClipTrack& track, const juce::String& name, const juce::File& file, te::TimeRange time,
                                        te::TimeDuration offset = {})
{
    auto& edit = track.edit;
    const auto start = fromTracktion (edit.tempoSequence.toBeats (time.getStart()));
    const auto from = offset.inSeconds();
    const auto clipState = makeAudioClipState (edit, name, file, start, { from, from + time.getLength().inSeconds() });
    return dynamic_cast<te::WaveAudioClip*> (placeClipState (edit, clipState, track));
}

// A note at a beat counted from the clip's content start, as Tracktion's MidiList::addNote() takes one.
inline te::MidiNote* addMidiNote (te::MidiClip& clip, int pitch, te::BeatPosition beat, te::BeatDuration length, int velocity = 100)
{
    return addNote (clip, pitch, clip.getContentStartBeat() + te::toDuration (beat), length, velocity);
}

// The IDs of Tracktion's clips, and the song of their Edit, for the model's functions that take clips by ID.
inline std::vector<ClipID> idsOf (const juce::Array<te::Clip*>& clips)
{
    std::vector<ClipID> ids;

    for (auto* clip : clips)
        if (clip != nullptr)
            ids.push_back (getClipID (*clip));

    return ids;
}

inline Song& songOf (const juce::Array<te::Clip*>& clips)
{
    REQUIRE_FALSE (clips.isEmpty());
    return getSong (clips.getFirst()->edit);
}
} // namespace phazite::test
