// SPDX-License-Identifier: AGPL-3.0-or-later
// Audio clips' own settings, as the audio clip editor and the clip commands change them (AudioClipModel.h says what each
// does to what is heard), and where a clip's whole audio file plays in the song. The song holds each audio clip on its
// track's element (Schema.h: an audio CLIP): its file, the part of the file it plays, in the file's own seconds, where it
// starts, in beats, and how it plays the part: following the song's tempo from its own, pitched, backwards. How long it
// lasts follows from those and the tempo. What plays it plays the file as it is, or else a render of it that Phazite makes
// (roadmap step 2.18; ClipRenders.h). None of these begins an undo step: their changes join the caller's.
#pragma once

#include "devices/stretch/Stretch.h"
#include "engine/audio/AudioRegion.h"
#include "model/arrangement/PlaylistValues.h"
#include "model/base/Beats.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"

#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
//==============================================================================
// The song's audio clips' elements.
bool isAudioState (const juce::ValueTree& clipState);

// The part of its file a clip plays, in seconds of the file as it was recorded, from < to whichever way it plays.
struct FilePart
{
    double start = 0.0, end = 0.0;
};

FilePart getFilePart (const juce::ValueTree& clipState);

// A new audio clip's element, with an ID of the song's, on no track yet: playing a part of a file from a beat.
juce::ValueTree makeAudioClipState (Song&, const juce::String& name, const juce::File&, BeatPosition start, FilePart);

// The file it plays (Song::getFile() is where a kept path starts), how long that file is in seconds, and how it plays it:
// at the song's tempo where it starts, or along the song's tempo changes (followsTempoChanges()).
juce::File getClipFile (const Song&, const juce::ValueTree& clipState);
double getClipFileLength (const Song&, const juce::ValueTree& clipState);
stretch::Settings getClipStretch (const Song&, const juce::ValueTree& clipState);

// Whether it follows the song's tempo where the tempo changes under its file, from the file's start to its end as the clip
// places them. Then each beat of its sound plays on a beat of the song, at the file's tempo from where the clip starts, so
// that it speeds up and slows down with the song, and its render follows a time map (stretch::Settings); under one tempo it
// plays at one ratio, as Phazite always played it.
bool followsTempoChanges (const Song&, const juce::ValueTree& clipState);

// How far into its file, in the file's seconds as it plays them, a move of its start or end takes it: from where that plays
// now, in the song's seconds, to a beat.
double getFileSecondsBetween (const Song&, const juce::ValueTree& clipState, double from, BeatPosition to);

// Where its part would start in its sound as it plays (backwards while it plays backwards), in seconds, if its start were
// so far into what plays it, in the song's seconds (PlayedPosition::offset): what a slip of it sets.
double getPartStartFor (const Song&, const juce::ValueTree& clipState, double offset);

// What plays for it: the file, or Phazite's render of it (getRenderFile()); and where, in the song's seconds, with how far
// into what plays it starts.
struct PlayedPosition
{
    double start = 0.0, end = 0.0, offset = 0.0;
};

juce::File getPlayedFile (const Song&, const juce::ValueTree& clipState);
PlayedPosition getPlayedPosition (const Song&, const juce::ValueTree& clipState);

// Where it is in beats: from its start for as long as its part takes to play.
BeatRange getAudioClipRange (const Song&, const juce::ValueTree& clipState);

// Where it starts and the part of its file it plays, in the caller's undo step.
void setAudioPart (Song&, juce::ValueTree clipState, BeatPosition start, FilePart);

// How long it fades in and out, in seconds, and the fades' shapes, in the caller's undo step; and the shape as Phazite's
// engine plays it.
double getClipFade (const juce::ValueTree& clipState, bool fadeIn);

// Its fades as they play, in seconds: as set, but scaled down together where they would overrun it.
std::pair<double, double> getFittedFades (const Song&, const juce::ValueTree& clipState);
void setClipFade (Song&, juce::ValueTree clipState, bool fadeIn, double seconds);
FadeCurve getClipFadeCurve (const juce::ValueTree& clipState, bool fadeIn);
void setClipFadeCurve (Song&, juce::ValueTree clipState, bool fadeIn, FadeCurve);
engine::FadeCurve toEngineFade (FadeCurve);

//==============================================================================
// The same by the clip's ID.

// How the clip plays its file, at the song's tempo where it starts: stretched to that from the clip's own tempo while it
// follows the song's, pitched, backwards. Unchanged for a clip that plays its file as it is, or one that isn't there.
stretch::Settings getClipStretch (const Song&, ClipID);

// Whether what plays it plays a render of the clip's file, which Phazite makes (ClipRenders.h).
bool isRendered (const Song&, ClipID);

// The file Phazite renders a file into with some settings: in the song's cache folder (a project's Cache folder), named
// for the file and the settings, so that clips that play a file alike share one.
juce::File getRenderFile (const Song&, const juce::File& source, const stretch::Settings&);

// What the start of the name of every render file is, to tell them from the rest of the Cache folder.
inline constexpr const char* renderFilePrefix = "phaziteClip_";

// Where the clip's whole file plays in the song, in seconds, as the clip plays it: from before the clip where its start
// was trimmed to after it where its end was.
struct SecondsRange
{
    double start = 0.0, end = 0.0;
};

SecondsRange getAudioFileRange (const Song&, const juce::ValueTree& clipState);

// The time, in seconds, in the clip's file (getClipFile()) that plays at a song beat as the clip plays it, or would play
// there if the clip reached it: a time outside the file before and after it. It goes down as the beats go up while the
// clip plays backwards. And the beat a time of the file plays at.
double getFileTimeAt (const Song&, ClipID, BeatPosition);
BeatPosition getBeatOfFileTime (const Song&, ClipID, double fileTime);
std::vector<BeatPosition> getBeatsOfFileTimes (const Song&, ClipID, const std::vector<double>& fileTimes); // at once

//==============================================================================
// Warp markers (Schema.h: WARP; AudioClipModel.h says what they do), changed in each clip of a clip's alias group, which
// plays the same audio, in the caller's undo step. A clip that doesn't follow the song's tempo follows it first; one that
// stops following it loses them.

// A clip's markers, each a time of its file as recorded with the beat it plays at, in the file's time order; none while it
// doesn't follow the song's tempo.
std::vector<WarpMarker> getWarpMarkers (const Song&, ClipID);

// A marker at a time of the file, where that plays now, so that nothing moves: false for a time outside the file, or within
// minWarpGap of a marker.
bool addWarpMarker (Song&, ClipID, double time);
inline constexpr double minWarpGap = 0.01; // seconds

// The marker at a time of the file to play at a beat, the audio between it and its neighbours stretched to fit, no faster
// or slower than 8 times; the audio where the clip starts stays there, held by a marker of its own while the moved one would
// move it (one at the clip's start doesn't move).
void moveWarpMarker (Song&, ClipID, double time, BeatPosition);

void removeWarpMarker (Song&, ClipID, double time);
void removeWarpMarkers (Song&, ClipID); // it plays straight at its tempo again

// Quantize Audio for one clip (AudioClipModel.h): each of the hits it plays (times of its file) towards the grid line nearest
// where it plays, by a share of the way (1 for all of it), with a marker of its own; the markers already there that no hit
// replaces stay where they play, and the audio where the clip starts stays there.
void quantizeClipAudio (Song&, ClipID, const std::vector<double>& hits, BeatDuration grid, double strength);

// The tempo the clip's audio was played at, in beats per minute, while it follows the song's tempo; 0 while it doesn't.
double getClipTempo (const Song&, ClipID);
float getClipPitch (const Song&, ClipID); // in semitones
bool isClipReversed (const Song&, ClipID);
float getClipGain (const Song&, ClipID); // in decibels

// Each in the caller's undo step; pitch, reverse and tempo in each clip of its alias group too.
void setClipGain (Song&, ClipID, float decibels);
void setClipPitch (Song&, ClipID, float semitones);
void setClipReversed (Song&, ClipID, bool);
void setClipFollowsTempo (Song&, ClipID, bool);
void setClipTempo (Song&, ClipID, double bpm);

// A clip recorded round a looped range (Schema.h: clipTakes): how many takes its file holds, 0 for a clip without any, and
// the one it plays (from 1); and playing another, the same part of it (in the caller's undo step).
int getClipTakeCount (const Song&, ClipID);
int getClipTake (const Song&, ClipID);
void playTake (Song&, ClipID, int take);

// The loudest sample of the part of its file a clip plays, in any of its channels, from 0 to 1 (more for a file of floating
// point samples that goes over); nothing for a clip whose file can't be read, or that isn't an audio clip. It reads that
// part of the file, on the caller's thread.
std::optional<float> getClipPeak (const Song&, ClipID);

// Each audio clip among them gets its gain set so that its loudest peak plays at normalizedPeak (AudioClipModel.h: what
// normalizeClip() does), in the caller's undo step; MIDI clips are left as they are.
void normalizeClips (Song&, const std::vector<ClipID>&);

// Every audio clip of the song, on every track.
std::vector<ClipID> getAudioClips (const Song&);

// The commands' switches for the selected clips: the audio clips among them play backwards, or forwards again if all of
// them do already; and follow the song's tempo, or stop if all of them do already. MIDI clips don't count.
bool areAllClipsReversed (const Song&, const std::vector<ClipID>&);
void toggleClipsReversed (Song&, const std::vector<ClipID>&);
bool doAllClipsFollowTempo (const Song&, const std::vector<ClipID>&);
void toggleClipsFollowTempo (Song&, const std::vector<ClipID>&);
} // namespace phazite
