// SPDX-License-Identifier: AGPL-3.0-or-later
// The metronome and the count-in (docs/design.md, section 17; roadmap step 1.7). The metronome is Phazite's engine's click
// (engine/transport/Click.h): a click on every beat of the time signature while the song plays (each eighth in 6/8, each
// half note in 2/2), a louder one on each bar's first. The song keeps whether it is on (Schema.h: metronome), as it keeps
// the loop, and switching it isn't an undo step. The count-in is a bar of clicks on the same beats before recording starts
// from a stop, heard with the metronome off too; it is the app's setting, for every project (model/engine/internal/AudioIO.h).
#pragma once

#include "model/base/Song.h"
#include "model/base/internal/Selection.h"

namespace phazite
{
class AudioIO;
class CommandRegistry;

bool isMetronomeOn (const Song&);
void setMetronomeOn (Song&, bool);
void toggleMetronome (Song&);

bool isCountInOn (const AudioIO&);
void setCountInOn (AudioIO&, bool);
void toggleCountIn (AudioIO&);

// The commands, for the selection's song and the app's audio: Metronome and Count-In, each on or off.
void addMetronomeCommands (CommandRegistry&, Selection&, AudioIO&);
} // namespace phazite
