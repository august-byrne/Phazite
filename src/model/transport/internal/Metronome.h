// SPDX-License-Identifier: AGPL-3.0-or-later
// The metronome and the count-in (docs/design.md, section 17; roadmap step 1.7). The metronome is Tracktion's click
// track: a click on every beat while the song plays, a louder one on each bar's first. The project keeps whether it is
// on, as it keeps the loop, and switching it isn't an undo step. The count-in is a bar of clicks before recording
// starts from a stop, heard with the metronome off too; it is the app's setting, for every project, which Tracktion
// keeps with its own.
#pragma once

#include "model/base/internal/Tracktion.h"

namespace phazite
{
class CommandRegistry;

bool isMetronomeOn (const te::Edit&);
void setMetronomeOn (te::Edit&, bool);
void toggleMetronome (te::Edit&);

bool isCountInOn (const te::Edit&);
void setCountInOn (te::Edit&, bool);
void toggleCountIn (te::Edit&);

// The commands, for the selection manager's Edit: Metronome and Count-In, each on or off.
void addMetronomeCommands (CommandRegistry&, te::SelectionManager&);
} // namespace phazite
