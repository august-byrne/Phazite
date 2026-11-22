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
#include "engine/core/TempoMap.h"
#include "model/arrangement/PlaylistValues.h"
#include "model/base/Beats.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/base/internal/SongValues.h"
#include "model/transport/TempoValues.h"

#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
//==============================================================================
// Whether a clip is an audio clip of the song's.
bool isAudioClip (const Song&, ClipID);

// The part of its file a clip plays, in seconds of the file as it was recorded, from < to whichever way it plays.
struct FilePart
{
    double start = 0.0, end = 0.0;
};

FilePart getFilePart (const ClipValue&);

// A fade of an audio clip's value as the playlist shows it (PlaylistValues.h).
FadeCurve getFadeCurve (const ClipFade&);

// A new audio clip's value, with an ID of the song's, on no track yet (its track the caller's to give; Clips.h: addClip()
// adds it): playing a part of a file from a beat, its start not before the song's and its file by the path the song keeps for
// it, relative to the project's folder.
ClipValue makeAudioClip (Song&, const juce::String& name, const juce::File&, BeatPosition start, FilePart);

// What plays for a clip: the file, or Phazite's render of it (getRenderFile()); and where, in the song's seconds, with how far
// into what plays it starts.
struct PlayedPosition
{
    double start = 0.0, end = 0.0, offset = 0.0;
};

// The song's tempo as audio clips' places need it, read once for many clips (a plan's compile reads it once for all the
// song's: known bug 124): its map in seconds (getTempoMap()) and its changes.
struct ClipTempo
{
    explicit ClipTempo (const Song&);

    engine::TempoMap map;
    std::vector<TempoChange> changes;
};

// What plays for a clip, each worked out once, as getPlayedFile(), getPlayedPosition() and getFittedFades() give them.
struct PlayedClip
{
    juce::File file;
    PlayedPosition position;
    double fadeIn = 0.0, fadeOut = 0.0;
};

// An audio clip's value, as the song holds it or as an edit is to give it (Clips.cpp and AudioClips.cpp work each edit out on
// values), worked out from the value alone, with what the song gives: its tempo, its folder and its files' lengths.
//
// The file it plays (Song::getFile() is where a kept path starts), how long that file is in seconds, and how it plays it: at
// the song's tempo where it starts, or along the song's tempo changes (followsTempoChanges()).
juce::File getClipFile (const Song&, const ClipValue&);
double getClipFileLength (const Song&, const ClipValue&);
stretch::Settings getClipStretch (const Song&, const ClipValue&);

// Whether it follows the song's tempo where the tempo changes under its file, from the file's start to its end as the clip
// places them. Then each beat of its sound plays on a beat of the song, at the file's tempo from where the clip starts, so
// that it speeds up and slows down with the song, and its render follows a time map (stretch::Settings); under one tempo it
// plays at one ratio, as Phazite always played it. False for a MIDI clip's value.
bool followsTempoChanges (const Song&, const ClipValue&);

// How far into its file, in the file's seconds as it plays them, a move of its start or end takes it: from where that plays
// now, in the song's seconds, to a beat.
double getFileSecondsBetween (const Song&, const ClipValue&, double from, BeatPosition to);

// Where its part would start in its sound as it plays (backwards while it plays backwards), in seconds, if its start were
// so far into what plays it, in the song's seconds (PlayedPosition::offset): what a slip of it sets.
double getPartStartFor (const Song&, const ClipValue&, double offset);

// What plays for it, and where (PlayedPosition); and all of what plays for it, each worked out once.
juce::File getPlayedFile (const Song&, const ClipValue&);
PlayedPosition getPlayedPosition (const Song&, const ClipValue&);
PlayedClip getPlayedClip (const Song&, const ClipValue&, const ClipTempo&);

// Where it is in beats: from its start for as long as its part takes to play.
BeatRange getAudioClipRange (const Song&, const ClipValue&);

// The shape of a fade as Phazite's engine plays it.
engine::FadeCurve toEngineFade (FadeCurve);

//==============================================================================
// The same of an audio clip's element: its kind, its part, a new one, the file it plays, how it plays it and where, and its
// fades, read from it (each that works something out reads the element into a value first: SongValues.h, readClipValue());
// and its part and a fade written into it, in the caller's undo step (only what differs: SongValues.h, writeClipElement()).
// For the tests and what reads a song's elements (the loader, a merge, a version applied: model/versions/); the clips' own
// code names clips by their IDs and values (roadmap step S5.20 takes these away once nothing names an element).
bool isAudioState (const juce::ValueTree& clipState);
FilePart getFilePart (const juce::ValueTree& clipState);
juce::ValueTree makeAudioClipState (Song&, const juce::String& name, const juce::File&, BeatPosition start, FilePart);
juce::File getClipFile (const Song&, const juce::ValueTree& clipState);
double getClipFileLength (const Song&, const juce::ValueTree& clipState);
stretch::Settings getClipStretch (const Song&, const juce::ValueTree& clipState);
bool followsTempoChanges (const Song&, const juce::ValueTree& clipState);
juce::File getPlayedFile (const Song&, const juce::ValueTree& clipState);
PlayedPosition getPlayedPosition (const Song&, const juce::ValueTree& clipState);
void setAudioPart (Song&, juce::ValueTree clipState, BeatPosition start, FilePart);
double getClipFade (const juce::ValueTree& clipState, bool fadeIn);
void setClipFade (Song&, juce::ValueTree clipState, bool fadeIn, double seconds);
FadeCurve getClipFadeCurve (const juce::ValueTree& clipState, bool fadeIn);

//==============================================================================
// The same by the clip's ID: of an audio clip of the song's, and for any other clip, or one that isn't there, nothing (an
// invalid file, 0, no fades, a curve).

juce::File getClipFile (const Song&, ClipID);
double getClipFileLength (const Song&, ClipID);
bool followsTempoChanges (const Song&, ClipID);
juce::File getPlayedFile (const Song&, ClipID);
PlayedPosition getPlayedPosition (const Song&, ClipID);

// Its fades as they play, in seconds: as set, but scaled down together where they would overrun it.
std::pair<double, double> getFittedFades (const Song&, ClipID);

// How the clip plays its file, at the song's tempo where it starts: stretched to that from the clip's own tempo while it
// follows the song's, pitched, backwards. Unchanged for a clip that plays its file as it is, or one that isn't there.
stretch::Settings getClipStretch (const Song&, ClipID);

// Whether what plays it plays a render of the clip's file, which Phazite makes (ClipRenders.h).
bool isRendered (const Song&, ClipID);

// The file Phazite renders a file into with some settings: in the song's cache folder (a project's Cache folder), named
// for the file's identity on the disk (its path, size and when it was written, asked at most once a second:
// model/engine/AudioFiles.h) and the settings, so that clips that play a file alike share one, and a file written again in
// its place has a render of its own made (the old one is never asked for again, and goes as the cache is purged:
// ClipRenders.h). Message thread.
juce::File getRenderFile (const Song&, const juce::File& source, const stretch::Settings&);

// What the start of the name of every render file is, to tell them from the rest of the Cache folder.
inline constexpr const char* renderFilePrefix = "phaziteClip_";

// Where the clip's whole file plays in the song, in seconds, as the clip plays it: from before the clip where its start
// was trimmed to after it where its end was.
struct SecondsRange
{
    double start = 0.0, end = 0.0;
};

SecondsRange getAudioFileRange (const Song&, const ClipValue&);
SecondsRange getAudioFileRange (const Song&, ClipID); // nothing for a clip that isn't an audio clip of the song's

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

// What the audio clips of an alias group share (Schema.h: clipLink): their file (clipFile, its path as the song holds it),
// which no edit changes in one clip alone (a clip is made with it, and Locate Audio File, model/project/ProjectContents.h,
// writes the path found into every clip that plays the old one); their tempo (clipTempo), pitch (clipPitch) and direction
// (clipReversed), which setClipPitch(), setClipReversed(), setClipFollowsTempo() and setClipTempo() write into each clip of
// the group; and their warp markers (WARP, times in that one file), which the markers' edits write into each; the rest of a
// clip is its own. Each of those edits gives every clip of the group the same value of all of them (SongValues.h:
// ClipSound). Whether a property is one of those; whether two audio clips' elements share them, compared as values,
// each property as read (its default where it has none: no tempo, no pitch, forwards) and the markers in their order, each as
// juce::ValueTree::isEquivalentTo() compares elements, the first's against the other's: as the loader (makeConsistent())
// and a merge (model/versions/internal/SongMerge.h) compare them; and the first's given to the other, in place of its own
// (a merge's keeper's, to every other clip of its group).
bool isSharedInAliasGroup (const juce::Identifier& property);
bool shareTheSound (const juce::ValueTree& first, const juce::ValueTree& other);
void giveSharedSound (const juce::ValueTree& from, juce::ValueTree to, juce::UndoManager*);

// A clip recorded round a looped range (Schema.h: clipTakes): how many takes its file holds, 0 for a clip without any, and
// the one it plays (from 1); and playing another, the same part of it (in the caller's undo step).
int getClipTakeCount (const Song&, ClipID);
int getClipTake (const Song&, ClipID);
void playTake (Song&, ClipID, int take);

// The loudest sample of the part of its file a clip plays, in any of its channels, from 0 to 1 (more for a file of floating
// point samples that goes over); nothing for a clip whose file can't be read, or that isn't an audio clip. It reads that
// part of the file, on the caller's thread.
std::optional<float> getClipPeak (const Song&, ClipID);

// The same of a part of a file, read a second at a time: no peak for a file that can't be read, and stopped (with none)
// once shouldStop says so; progress hears how much is read, from 0 to 1. Any thread: what Normalize reads in the
// background (AudioWork.h).
struct PeakRead
{
    std::optional<float> peak;
    bool stopped = false;
};

PeakRead readPeak (const juce::File&, FilePart, const std::function<bool()>& shouldStop, const std::function<void (float)>& progress);

// Every audio clip of the song, on every track.
std::vector<ClipID> getAudioClips (const Song&);

// The paths of the audio files the song plays, as the song holds them (a path within the project's folder, or a full one),
// each once: its clips' files, read by their IDs (getClipValues(), whatever holds the clips), in the song's order, track by
// track; then its devices' sound files (Schema.h: isSoundFileProperty()), in the order its tree holds them. What Clean Up
// Unused Audio keeps (model/project/UnusedAudio.h), and what an import keeps of the audio it unpacked (model/project/internal/
// DawProject.h: forgetUnusedAudio()). Never the song's tree read for its clips: the song is to hold them outside it (roadmap
// step S5.20). For a whole song's tree (a copy, a file read), model/versions/internal/VersionStore.h has its own.
juce::StringArray findAudioFilePaths (const Song&);

// The commands' switches for the selected clips: the audio clips among them play backwards, or forwards again if all of
// them do already; and follow the song's tempo, or stop if all of them do already. MIDI clips don't count.
bool areAllClipsReversed (const Song&, const std::vector<ClipID>&);
void toggleClipsReversed (Song&, const std::vector<ClipID>&);
bool doAllClipsFollowTempo (const Song&, const std::vector<ClipID>&);
void toggleClipsFollowTempo (Song&, const std::vector<ClipID>&);
} // namespace phazite
