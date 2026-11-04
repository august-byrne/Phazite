// SPDX-License-Identifier: AGPL-3.0-or-later
// Recording (docs/design.md, section 17; roadmap steps 1.7 and 3.4). What MIDI keyboards and the computer keyboard play is
// recorded onto the track live MIDI plays (LiveInput.h), with no track to arm first, and the armed tracks record their
// audio inputs (RecordingModel.h, which Phazite records itself: AudioRecorder.h, and through its own engine, Take.h): from
// where playback starts, after the count-in when it is on (Metronome.h), or from where the song is if it plays already.
// With a range looped, recording goes round with it, each time round a take of an audio clip; with Punch on, only what is
// played in the looped range is kept, the take not looping. Overdub, the default, adds the notes to the clips already
// there; with it off, they take those clips' place. Recorded notes can be quantized to sixteenths as they go in. However
// recording stops, the take is one undo step, "Record" (Playback.h's stop functions keep it too). Overdub and quantizing
// are the app's settings, for every project, which Phazite keeps (model/engine/internal/AudioIO.h). The song records
// through its playback session (model/playback/internal/PlaybackSession.h) while Phazite's engine plays it: without one,
// nothing records.
#pragma once

#include "model/base/Song.h"
#include "model/base/internal/Selection.h"

namespace phazite
{
class AudioIO;
class CommandRegistry;

// Whether recording can start: the engine plays the song, and live MIDI plays one of its tracks, or a track is armed with
// an input.
bool canRecord (const Song&);
bool isRecording (const Song&);

// Starts recording, playing from where playback starts (after the count-in, when it is on), or from where the song is
// if it plays already. Nothing if it can't.
void startRecording (Song&);

// Stops recording and playback, back where playback started, and keeps what was played as one undo step, "Record".
void stopRecording (Song&);

// The Record command: stops a recording, or starts one if it can.
void toggleRecording (Song&);
bool canToggleRecording (const Song&);

bool isOverdubOn (const AudioIO&);
void setOverdubOn (AudioIO&, bool);
void toggleOverdub (AudioIO&);

bool isRecordQuantizeOn (const AudioIO&);
void setRecordQuantizeOn (AudioIO&, bool);
void toggleRecordQuantize (AudioIO&);

// Punch (RecordingModel.h): the project's switch, kept with it like the loop's, not an undo step; it punches only while a
// range is looped on purpose (canPunch()), and shows as on only then.
bool isPunchOn (const Song&);
bool canPunch (const Song&);
void setPunchOn (Song&, bool);
bool isPunching (const Song&); // on, and a range looped
void togglePunch (Song&);

// Arm Track: the selected tracks that can record audio and have inputs armed, or disarmed if they all were.
bool canArmSelectedTracks (const Selection&);
bool areSelectedTracksArmed (const Selection&);
void toggleArmSelectedTracks (const Selection&);

// Next Take and Previous Take: each selected clip with takes plays its next or previous one, in the caller's undo step.
bool canStepTakes (const Selection&);
void stepTakes (const Selection&, int direction);

// The commands, for the selection's song and the app's audio: Record, Overdub, Quantize Recordings, Punch In, Arm Track,
// and Next and Previous Take.
void addRecordingCommands (CommandRegistry&, Selection&, AudioIO&);
} // namespace phazite
