// SPDX-License-Identifier: AGPL-3.0-or-later
// Audio clips' own settings, as the audio clip editor and the clip commands change them (AudioClipModel.h says what each
// does to what is heard), and where a clip's whole audio file plays in the song. The song holds each audio clip on its
// track's element (Schema.h: an audio CLIP): its file, the part of the file it plays, in the file's own seconds, where it
// starts, in beats, and how it plays the part: following the song's tempo from its own, pitched, backwards. How long it
// lasts follows from those and the tempo. Tracktion's clip mirrors it (model/mirror/internal/Mirror.h), playing the file
// as it is, or else a render of it that Phazite makes itself (roadmap step 2.18; ClipRenders.h). None of these begins an
// undo step: their changes join the caller's.
#pragma once

#include "devices/stretch/Stretch.h"
#include "model/base/internal/Tracktion.h"

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

// A new audio clip's element, with an ID from the Edit's, on no track yet: playing a part of a file from a beat.
juce::ValueTree makeAudioClipState (te::Edit&, const juce::String& name, const juce::File&, BeatPosition start, FilePart);

// The file it plays, how long that file is in seconds, and how it plays it: at the song's tempo where it starts.
juce::File getClipFile (const te::Edit&, const juce::ValueTree& clipState);
double getClipFileLength (const te::Edit&, const juce::ValueTree& clipState);
stretch::Settings getClipStretch (const te::Edit&, const juce::ValueTree& clipState);

// What Tracktion plays for it: the file, or Phazite's render of it; and where, in seconds, its offset into what it plays.
juce::File getPlayedFile (te::Edit&, const juce::ValueTree& clipState);
te::ClipPosition getPlayedPosition (const te::Edit&, const juce::ValueTree& clipState);

// Where it is in beats: from its start for as long as its part takes to play.
BeatRange getAudioClipRange (const te::Edit&, const juce::ValueTree& clipState);

// Where it starts and the part of its file it plays, in the caller's undo step, and Tracktion's clip follows.
void setAudioPart (te::Edit&, juce::ValueTree clipState, BeatPosition start, FilePart);

// How long it fades in and out, in seconds, and the fades' shapes, as Tracktion's types.
double getClipFade (const juce::ValueTree& clipState, bool fadeIn);
te::AudioFadeCurve::Type getFadeShape (const juce::ValueTree& clipState, bool fadeIn);
void setClipFade (te::Edit&, juce::ValueTree clipState, bool fadeIn, double seconds);
juce::String getSavedFadeShape (te::AudioFadeCurve::Type); // as the song saves it (Schema.h: fadeIsLinear...)

//==============================================================================
// The same, from Tracktion's clip.

// The audio file a clip's sound comes from.
juce::File getClipFile (const te::AudioClipBase&);

// How the clip plays its file, at the song's tempo where it starts: stretched to that from the clip's own tempo while it
// follows the song's, pitched, backwards. Unchanged for a clip that plays its file as it is.
stretch::Settings getClipStretch (const te::AudioClipBase&);

// Whether Tracktion plays a render of the clip's file, which Phazite makes (ClipRenders.h).
bool isRendered (const te::AudioClipBase&);

// The file Phazite renders a file into with some settings: in the Edit's temporary folder (a project's Cache folder),
// named for the file and the settings, so that clips that play a file alike share one.
juce::File getRenderFile (te::Edit&, const juce::File& source, const stretch::Settings&);

// What the start of the name of every render file is, to tell them from the rest of the Cache folder.
inline constexpr const char* renderFilePrefix = "phaziteClip_";

// Where the clip's whole file plays in the song as the clip plays it: from before the clip where its start was trimmed to
// after it where its end was.
te::TimeRange getAudioTimeRange (te::AudioClipBase&);

// The time, in seconds, in the clip's file (getClipFile()) that plays at a song beat as the clip plays it, or would play
// there if the clip reached it: a time outside the file before and after it. It goes down as the beats go up while the
// clip plays backwards.
double getFileTimeAt (te::AudioClipBase&, te::BeatPosition);

// The tempo the clip's audio was played at, in beats per minute, while it follows the song's tempo; 0 while it doesn't.
double getClipTempo (const te::AudioClipBase&);
float getClipPitch (const te::AudioClipBase&); // in semitones
bool isClipReversed (const te::AudioClipBase&);
float getClipGain (const te::AudioClipBase&); // in decibels

void setClipGain (te::AudioClipBase&, float decibels);
void setClipPitch (te::AudioClipBase&, float semitones);
void setClipReversed (te::AudioClipBase&, bool);
void setClipFollowsTempo (te::AudioClipBase&, bool);
void setClipTempo (te::AudioClipBase&, double bpm);

// A clip recorded round a looped range (Schema.h: clipTakes): how many takes its file holds, 0 for a clip without any, and
// the one it plays (from 1); and playing another, the same part of it (in the caller's undo step).
int getClipTakeCount (const te::AudioClipBase&);
int getClipTake (const te::AudioClipBase&);
void playTake (te::AudioClipBase&, int take);

// Every audio clip in an Edit, on every track.
juce::Array<te::AudioClipBase*> getAudioClips (te::Edit&);

// The commands' switches for the selected clips: the audio clips among them play backwards, or forwards again if all of
// them do already; and follow the song's tempo, or stop if all of them do already. MIDI clips don't count.
bool areAllClipsReversed (const juce::Array<te::Clip*>&);
void toggleClipsReversed (const juce::Array<te::Clip*>&);
bool doAllClipsFollowTempo (const juce::Array<te::Clip*>&);
void toggleClipsFollowTempo (const juce::Array<te::Clip*>&);
} // namespace phazite
