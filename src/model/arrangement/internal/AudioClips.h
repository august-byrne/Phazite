// SPDX-License-Identifier: AGPL-3.0-or-later
// Audio clips' own settings, as the audio clip editor and the clip commands change them (AudioClipModel.h says what each
// does to what is heard), and where a clip's whole audio file plays in the song. None of these begins an undo step: their
// changes join the caller's.
#pragma once

#include "model/base/internal/Tracktion.h"

namespace phazite
{
// Where the clip's whole file plays in the song as the clip plays it: from before the clip where its start was trimmed to
// after it where its end was.
te::TimeRange getAudioTimeRange (te::AudioClipBase&);

// The time, in seconds, in the file the clip's audio comes from (getAudioFile(): its file, or a reversed copy of it) that
// plays at a song beat as the clip plays it, or would play there if the clip reached it: a time outside the file before
// and after it. Not a time in the proxy Tracktion plays a stretched or pitched clip from, which starts where the clip
// does and holds only what the clip plays.
double getFileTimeAt (te::AudioClipBase&, te::BeatPosition);

// The tempo the clip's audio was played at, in beats per minute, while it follows the song's tempo; 0 while it doesn't.
double getClipTempo (te::AudioClipBase&);

void setClipGain (te::AudioClipBase&, float decibels);
void setClipPitch (te::AudioClipBase&, float semitones);
void setClipReversed (te::AudioClipBase&, bool);
void setClipFollowsTempo (te::AudioClipBase&, bool);
void setClipTempo (te::AudioClipBase&, double bpm);

// The commands' switches for the selected clips: the audio clips among them play backwards, or forwards again if all of
// them do already; and follow the song's tempo, or stop if all of them do already. MIDI clips don't count.
bool areAllClipsReversed (const juce::Array<te::Clip*>&);
void toggleClipsReversed (const juce::Array<te::Clip*>&);
bool doAllClipsFollowTempo (const juce::Array<te::Clip*>&);
void toggleClipsFollowTempo (const juce::Array<te::Clip*>&);
} // namespace phazite
