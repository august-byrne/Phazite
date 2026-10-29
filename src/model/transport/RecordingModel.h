// SPDX-License-Identifier: AGPL-3.0-or-later
// Recording audio as the track headers, the mixer, the transport bar and the playlist reach it (docs/design.md, section 17;
// roadmap step 3.4), by IDs and plain values (no Tracktion here; internal/AudioRecorder.h is its Tracktion side).
//
// A track records audio while it is armed, from its input: one of the audio device's inputs, alone (mono) or a pair
// (stereo). Only a track without an instrument (not a folder or a return) can be armed: a track with one plays what is
// played on MIDI, unarmed, as before. Arming is neither saved (a project opens with nothing armed, so that no microphone
// feeds back as it opens) nor an undo step. A track's input, and whether it is heard while armed, always or never, are
// saved, each change one named undo step. Record (internal/Recording.h) records the armed tracks with what is played on
// MIDI, all as one undo step. With Punch on, only what falls in the looped range is kept, in place of what was there;
// recording round a looped range makes takes of one clip, the newest whole one playing.
#pragma once

#include "model/base/IDs.h"

#include <juce_core/juce_core.h>

#include <optional>
#include <vector>

namespace phazite
{
class ProjectContext;

// One of the audio device's inputs, as a track records from it: a channel alone ("1", named "In 1") or a pair of them
// ("1+2", "In 1+2"), counted from 1.
struct AudioInput
{
    juce::String id;
    juce::String name;
    int numChannels = 1;
};

// The open audio device's inputs: each channel alone, then each pair (1+2, 3+4...); none without an audio device, or
// one without inputs.
std::vector<AudioInput> getAudioInputs (const ProjectContext&);

// "In 1" for "1": an input's name, or "No input" for none.
juce::String describeInput (const juce::String& inputID);

// Whether a track can record audio: a track, not a folder or a return, without an instrument.
bool canRecordAudio (const ProjectContext&, TrackID);

// A track's input, by its id, or empty for none; and the input chosen last on any track, which its list shows first.
juce::String getTrackInput (const ProjectContext&, TrackID);
juce::String getLastInput (const ProjectContext&);

// "Change Input": a track's input, or none (an empty id), as one undo step. A track left without one is disarmed.
void setTrackInput (ProjectContext&, TrackID, const juce::String& inputID);

// When a track's input is heard through it (monitoring): while it is armed (the default), always, or never (for an audio
// interface that lets its player hear the input directly).
enum class InputMonitoring
{
    whileArmed,
    always,
    never
};

InputMonitoring getInputMonitoring (const ProjectContext&, TrackID);
void setInputMonitoring (ProjectContext&, TrackID, InputMonitoring); // "Change Input Monitoring"

// Arming: a track with an input records it when Record is pressed. Not an undo step, not saved. setArmed() does nothing
// for a track that can't record audio or has no input; armOnly() arms one and disarms every other.
bool isArmed (const ProjectContext&, TrackID);
void setArmed (ProjectContext&, TrackID, bool);
void armOnly (ProjectContext&, TrackID);
std::vector<TrackID> getArmedTracks (const ProjectContext&); // in the song's order

// Punch: Record keeps only what is played in the looped range, in place of what was there, with short crossfades at its
// edges. Saved with the project, as the loop is, but not an unsaved change. It needs a range looped on purpose: while
// the loop follows the whole song it is off.
bool isPunchOn (const ProjectContext&);
bool canPunch (const ProjectContext&);
void setPunchOn (ProjectContext&, bool);

// An audio clip recorded round a looped range: its takes, one for each time round, and which one plays (from 1); none
// for a clip without takes.
struct ClipTakes
{
    int count = 0;
    int playing = 0;
};

ClipTakes getClipTakes (const ProjectContext&, ClipID);

// "Choose Take": the take a clip plays, from 1, as one undo step.
void setClipTake (ProjectContext&, ClipID, int take);
} // namespace phazite
