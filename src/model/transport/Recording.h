// SPDX-License-Identifier: AGPL-3.0-or-later
// Recording MIDI (docs/design.md, section 17; roadmap step 1.7). What MIDI keyboards and the computer keyboard play is
// recorded onto the track live MIDI plays (LiveInput.h), with no track to arm first: from where playback starts, after
// the count-in when it is on (Metronome.h), or from where the song is if it plays already. With the loop on, recording
// goes round with it. Overdub, the default, adds the notes to the clips already there; with it off, they take those
// clips' place. Recorded notes can be quantized to sixteenths as they go in. However recording stops, the take is one
// undo step, "Record" (Playback.h's stop functions keep it too). Overdub and quantizing are the app's settings, for every
// project, which Tracktion keeps with the MIDI input's own.
#pragma once

#include "model/base/Tracktion.h"

namespace phazite
{
class CommandRegistry;

// Whether recording can start: live MIDI plays one of the Edit's tracks.
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

// The commands, for the selection manager's Edit: Record, Overdub and Quantize Recordings.
void addRecordingCommands (CommandRegistry&, te::SelectionManager&);
} // namespace phazite
