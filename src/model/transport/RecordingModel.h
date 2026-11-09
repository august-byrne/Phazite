// SPDX-License-Identifier: AGPL-3.0-or-later
// Recording audio as the track headers, the mixer, the transport bar and the playlist reach it (docs/design.md, section 17;
// roadmap step 3.4), by IDs and plain values (internal/AudioRecorder.h is the model's side of it).
//
// A track records audio while it is armed, from its input: one of the audio device's inputs, alone (mono) or a pair
// (stereo). Only a track without an instrument (not a folder or a return) can be armed: a track with one plays what is
// played on MIDI, unarmed, as before. Arming is neither saved (a project opens with nothing armed, so that no microphone
// feeds back as it opens) nor an undo step. A track's input, and whether it is heard while armed, always or never, are
// saved, each change one named undo step. Record (internal/Recording.h) records the armed tracks with what is played on
// MIDI, all as one undo step. With Punch on, only what falls in the looped range is kept, in place of what was there;
// recording round a looped range makes takes of one clip, the newest whole one playing. While it records, what has come in
// shows where it will go, growing on each track as it records (getRecordingsInProgress()), as Ableton Live draws it.
#pragma once

#include "RecordingValues.h"
#include "model/base/Beats.h"
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

//==============================================================================
// What is being recorded now (RecordingValues.h), where it will go, for the playlist to draw on each track as it comes in,
// as Ableton Live draws a clip growing as it records. Nothing while nothing records.
std::vector<RecordingInProgress> getRecordingsInProgress (const ProjectContext&);

// An armed track's input so far, as its waveform: the loudest sample in each of so many columns across a stretch of the
// song, from 0 to 1, as this time round a looped range plays it; 0 where nothing has come in yet, or nothing records.
std::vector<float> getRecordingLevels (const ProjectContext&, TrackID, BeatRange span, int columns);

//==============================================================================
// Recording MIDI (docs/design.md, section 17): what is played on MIDI keyboards and the computer keyboard records onto the
// track live MIDI plays (TransportModel.h: getLiveMidiTrack(): the selected track with an instrument, as FL Studio's
// selected channel), armed as live MIDI starts playing it, as Ableton Live arms the selected MIDI track by itself. A track
// with an instrument has an arm switch that shows it: a click on another's sends live MIDI there, armed; on the one armed,
// it stops recording onto it (it still plays), until a click arms it again or live MIDI moves to another. Neither saved nor
// an undo step.
bool canRecordMidi (const ProjectContext&, TrackID); // a track, not a folder or a return, with an instrument
bool isMidiArmed (const ProjectContext&, TrackID);   // live MIDI plays it, and Record records onto it
void setMidiArmed (ProjectContext&, TrackID, bool);

// What Record records onto now, in the song's order, as the tracks' arm switches show it: each armed track's input that
// the audio device has, and live MIDI's track while it is armed; and those as words, for Record's tooltip, "Lead (MIDI) and
// Vocals (In 1)", empty while it would record nothing.
struct RecordTarget
{
    TrackID track {};
    RecordSource source = RecordSource::input;
    juce::String input {}; // an input's id ("1", "1+2")
};

std::vector<RecordTarget> getRecordTargets (const ProjectContext&);
juce::String describeRecordTargets (const ProjectContext&);

// Whether Record is recording onto a track now, from an input or MIDI: its header and strip say so.
bool isRecordingOnto (const ProjectContext&, TrackID);

// "Add Audio Track": a track without an instrument after the selected one (or at the end), named "Audio" (then "Audio 2"...),
// recording one of the audio device's inputs, armed, and selected alone, as one undo step: for a microphone or an instrument
// plugged in, which Record then records. Nothing for an input the device doesn't have. Returns the track, or none.
TrackID addTrackForInput (ProjectContext&, const juce::String& inputID);
} // namespace phazite
