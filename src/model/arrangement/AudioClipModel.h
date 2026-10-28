// SPDX-License-Identifier: AGPL-3.0-or-later
// An audio clip as the audio clip editor shows it (docs/design.md, section 7; roadmap step 2.6), by IDs and plain values
// (no Tracktion here): its audio file, where the whole file plays in the song, and how the clip plays it: its gain, its
// pitch, backwards or not, and whether it follows the song's tempo. Each function takes the open project's context, and
// gives nothing, or does nothing, for an ID that isn't an audio clip's. Each change is an undo step of its own, named as
// its comment says; while a gesture is open (SongGesture.h), as while a field is dragged, it joins the gesture's step.
// Trims, fades and slips are the playlist's edits (PlaylistModel.h), which the editor makes too.
#pragma once

#include "PlaylistModel.h"

#include <optional>

namespace phazite
{
// What the editor offers. Tracktion's clips reach further down (to -100 dB), and as far in pitch.
inline constexpr float minClipGain = -60.0f, maxClipGain = 24.0f;  // decibels
inline constexpr float maxClipPitch = 48.0f;                       // semitones, up or down
inline constexpr double minClipTempo = 20.0, maxClipTempo = 300.0; // beats per minute, as the song's tempo

enum class AudioState
{
    ready,     // it plays
    preparing, // Phazite is rendering it stretched, pitched or reversed in the background: the clip is silent until then
    missing    // its audio file can't be found
};

struct AudioClipInfo
{
    ClipInfo clip {};         // what the playlist shows of it: its track, name, place and fades
    juce::String fileName {}; // its audio file's, with the extension
    double sampleRate = 0.0;  // the file's, in hertz
    int channels = 0;
    double seconds = 0.0; // how long the file lasts at its own speed

    // Where the whole file plays in the song, as the clip plays it: from before the clip where its start was trimmed to
    // after it where its end was.
    BeatRange content {};

    float gain = 0.0f;  // in decibels
    float pitch = 0.0f; // in semitones, up or down, with cents as hundredths; the clip keeps its length
    bool reversed = false;

    // Whether it stretches to follow the song's tempo (its pitch staying the same), and then the tempo its audio was
    // played at, in beats per minute, which plays at the song's tempo; 0 while it doesn't follow.
    bool followsTempo = false;
    double tempo = 0.0;

    AudioState state = AudioState::ready;
};

std::optional<AudioClipInfo> getAudioClip (const ProjectContext&, ClipID);

// The audio clip on a track at a beat, or else the one nearest to it; nothing on a track without audio clips.
std::optional<ClipID> getAudioClipNearest (const ProjectContext&, TrackID, BeatPosition);

// What the song's selection asks the editor dock's first tab to show (docs/design.md, section 4): with MIDI clips
// selected, the piano roll; else, with audio clips, the audio clip editor; else, for a selected track, the audio clip
// editor if the track holds audio, or else the piano roll. Nothing for any other selection: what shows stays.
std::optional<ClipType> getClipTypeToEdit (const ProjectContext&);

// The audio clip the selection asks the audio clip editor to show: the first audio clip selected, or a selected track's
// audio clip at a beat (the playhead's), or else the one nearest to it. Nothing while MIDI clips are selected.
std::optional<ClipID> getAudioClipToEdit (const ProjectContext&, BeatPosition);

//==============================================================================
// "Change Clip Gain": in decibels, kept within the editor's range.
void setClipGain (ProjectContext&, ClipID, float decibels);

// "Change Clip Pitch": in semitones, with cents as hundredths, kept within the editor's range. The clip keeps its length.
void setClipPitch (ProjectContext&, ClipID, float semitones);

// "Reverse Clip", or "Play Clip Forwards": the same part of its audio plays, backwards, where the clip is.
void setClipReversed (ProjectContext&, ClipID, bool);

// "Follow Tempo", or "Stop Following Tempo". Following, the clip stretches with the song's tempo, from the tempo its file
// gives, or else the song's tempo where the clip starts, so that it sounds as it did until either tempo changes. Either
// way the clip keeps its start and the part of its audio it plays, so its length changes with its speed.
void setClipFollowsTempo (ProjectContext&, ClipID, bool);

// "Change Clip Tempo": the tempo its audio was played at, while it follows the song's, kept within the editor's range. The
// clip keeps its start and the part of its audio it plays, so it gets shorter or longer.
void setClipTempo (ProjectContext&, ClipID, double bpm);
} // namespace phazite
