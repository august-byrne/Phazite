// SPDX-License-Identifier: AGPL-3.0-or-later
// Capture (docs/design.md, section 17; roadmap step 1.7), as Ableton Live's Capture MIDI and Logic Pro's Capture as
// Recording: Phazite keeps the last 30 seconds of what MIDI keyboards and the computer keyboard played (Tracktion's
// retrospective buffer), recording or not, and Capture turns them into a clip on the track live MIDI plays, as one undo
// step, "Capture", and selects it.
// - While the song plays, the notes stay where they were played in it.
// - While it is stopped, the clip goes at the bar the play start is in, or after the clips already there, with the
//   first note on the bar's first beat. In a song without clips, Capture first sets the song's tempo from what was
//   played, when it can tell (detectTempo()).
// The clip runs whole bars, and its notes are quantized to sixteenths when recordings are (Recording.h).
#pragma once

#include "model/base/internal/Tracktion.h"

#include <optional>
#include <vector>

namespace phazite
{
class CommandRegistry;

// Whether Capture can make a clip: live MIDI plays one of the Edit's tracks.
bool canCapture (te::Edit&);

// Makes the clip; nullptr if nothing was played. With a selection manager, the clip is selected.
te::MidiClip* capture (te::Edit&, te::SelectionManager*);

// The tempo notes were played at, from when they began (in seconds, in order), to a whole beat per minute: the one
// between 80 and 160 at which they fall best on a grid of sixteenths, with more of them on beats, from the first note.
// Nothing for fewer than four notes, for less than a second of them, or when no tempo fits them well.
std::optional<double> detectTempo (const std::vector<double>& noteStartSeconds);

// The command, for the selection manager's Edit: Capture.
void addCaptureCommands (CommandRegistry&, te::SelectionManager&);
} // namespace phazite
