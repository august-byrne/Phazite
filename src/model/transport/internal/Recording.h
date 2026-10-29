// SPDX-License-Identifier: AGPL-3.0-or-later
// Recording (docs/design.md, section 17; roadmap steps 1.7 and 3.4). What MIDI keyboards and the computer keyboard play is
// recorded onto the track live MIDI plays (LiveInput.h), with no track to arm first, and the armed tracks record their
// audio inputs (RecordingModel.h, which Phazite records itself: AudioRecorder.h): from where playback starts, after the
// count-in when it is on (Metronome.h), or from where the song is if it plays already. With a range looped, recording
// goes round with it, each time round a take of an audio clip; with Punch on, only what is played in the looped range is
// kept, the take not looping. Overdub, the default, adds the notes to the clips already there; with it off, they take
// those clips' place. Recorded notes can be quantized to sixteenths as they go in. However recording stops, the take is
// one undo step, "Record" (Playback.h's stop functions keep it too). Overdub and quantizing are the app's settings, for
// every project, which Tracktion keeps with the MIDI input's own.
#pragma once

#include "model/base/internal/Tracktion.h"

namespace phazite
{
class CommandRegistry;

// Whether recording can start: live MIDI plays one of the Edit's tracks, or a track is armed with an input.
bool canRecord (te::Edit&);
bool isRecording (const te::Edit&);

// Starts recording, playing from where playback starts (after the count-in, when it is on), or from where the song is
// if it plays already. Nothing if it can't.
void startRecording (te::Edit&);

// Stops recording and playback, back where playback started, and keeps what was played as one undo step, "Record".
void stopRecording (te::Edit&);

// The Record command: stops a recording, or starts one if it can.
void toggleRecording (te::Edit&);
bool canToggleRecording (te::Edit&);

bool isOverdubOn (te::Engine&);
void setOverdubOn (te::Engine&, bool);
void toggleOverdub (te::Engine&);

bool isRecordQuantizeOn (te::Engine&);
void setRecordQuantizeOn (te::Engine&, bool);
void toggleRecordQuantize (te::Engine&);

// Punch (RecordingModel.h): the project's switch, kept with it like the loop's, not an undo step; it punches only while a
// range is looped on purpose (canPunch()), and shows as on only then.
bool isPunchOn (const te::Edit&);
bool canPunch (const te::Edit&);
void setPunchOn (te::Edit&, bool);
bool isPunching (const te::Edit&); // on, and a range looped
void togglePunch (te::Edit&);

// Arm Track: the selected tracks that have inputs armed, or disarmed if they all were.
bool canArmSelectedTracks (const te::SelectionManager&);
bool areSelectedTracksArmed (const te::SelectionManager&);
void toggleArmSelectedTracks (const te::SelectionManager&);

// Next Take and Previous Take: each selected clip with takes plays its next or previous one, in the caller's undo step.
bool canStepTakes (const te::SelectionManager&);
void stepTakes (const te::SelectionManager&, int direction);

// The commands, for the selection manager's Edit: Record, Overdub, Quantize Recordings, Punch In, Arm Track, and Next and
// Previous Take.
void addRecordingCommands (CommandRegistry&, te::SelectionManager&);
} // namespace phazite
