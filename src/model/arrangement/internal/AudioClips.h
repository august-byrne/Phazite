// SPDX-License-Identifier: AGPL-3.0-or-later
// Audio clips' own settings, as the audio clip editor and the clip commands change them (AudioClipModel.h says what each
// does to what is heard), and where a clip's whole audio file plays in the song. A clip that follows the song's tempo,
// or is pitched or reversed, plays a render of its file that Phazite makes itself (roadmap step 2.18): Tracktion plays
// the render as a plain clip, and the clip's Phazite properties (Schema.h: phaziteSource and the rest) say what it is
// made from. ClipRenders.h makes the renders. None of these begins an undo step: their changes join the caller's.
#pragma once

#include "devices/stretch/Stretch.h"
#include "model/base/internal/Tracktion.h"

#include <vector>

namespace phazite
{
// The audio file a clip's sound comes from: the one Phazite renders it from, or else the one Tracktion plays.
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

void setClipGain (te::AudioClipBase&, float decibels);
void setClipPitch (te::AudioClipBase&, float semitones);
void setClipReversed (te::AudioClipBase&, bool);
void setClipFollowsTempo (te::AudioClipBase&, bool);
void setClipTempo (te::AudioClipBase&, double bpm);

// Every audio clip in an Edit, on every track.
juce::Array<te::AudioClipBase*> getAudioClips (te::Edit&);

// Keeps the clips that follow the song's tempo in time while it changes (model/transport/internal/Tempo.h: setTempo()):
// made before the change, it lays each of them out again afterwards, from where its start is then, stretched for the new
// tempo to play the same part of its file. Tracktion moves a clip's start with its beat, but keeps a plain clip's length,
// which Phazite's clips all are to Tracktion.
class FollowingClipsOnBeats
{
public:
    explicit FollowingClipsOnBeats (te::Edit&);
    ~FollowingClipsOnBeats();

private:
    struct Kept
    {
        te::SafeSelectable<te::AudioClipBase> clip;
        double fileStart = 0.0, fileEnd = 0.0; // the part of its file it plays, in seconds of the file
    };

    std::vector<Kept> kept;

    JUCE_DECLARE_NON_COPYABLE (FollowingClipsOnBeats)
};

// The commands' switches for the selected clips: the audio clips among them play backwards, or forwards again if all of
// them do already; and follow the song's tempo, or stop if all of them do already. MIDI clips don't count.
bool areAllClipsReversed (const juce::Array<te::Clip*>&);
void toggleClipsReversed (const juce::Array<te::Clip*>&);
bool doAllClipsFollowTempo (const juce::Array<te::Clip*>&);
void toggleClipsFollowTempo (const juce::Array<te::Clip*>&);
} // namespace phazite
