// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's tempo and time signature (docs/design.md, section 4; roadmap step 1.7), which the top bar shows and
// changes. For now a song has one of each, from its start; changes along the song come later, with a tempo lane. Clips
// stay on their beats when the tempo changes, as the playlist's time line is in beats: those that follow the tempo
// stretch with it, the others keep their speed (TempoStretch, TempoValues.h).
#pragma once

#include "model/base/internal/Tracktion.h"
#include "model/transport/TempoValues.h"

namespace phazite
{
class CommandRegistry;

double getTempo (const te::Edit&);

// Changes the tempo, as one undo step, "Change Tempo" (a drag's changes join the drag's step). It stays within the
// limits (TempoValues.h), to a hundredth of a beat per minute.
void setTempo (te::Edit&, double bpm);

// What a change of the tempo stretches.
TempoStretch getTempoStretch (te::Edit&);

// The Samples Follow Tempo command's switch: on while every sample in the song follows the tempo (and there is one).
// Toggled, every sample follows it, or none does if all did (model/arrangement/internal/AudioClips.h).
bool doAllSamplesFollowTempo (te::Edit&);
void toggleSamplesFollowTempo (te::Edit&);

// The Patterns Follow Tempo command's switch, the song's (schema::patternsKeepTheirSpeed): on, a tempo change leaves
// MIDI clips' notes on their beats, so they play faster or slower; off, it moves the notes to keep their times.
bool doPatternsFollowTempo (const te::Edit&);
void togglePatternsFollowTempo (te::Edit&);

// Registers the commands that say what a tempo change stretches.
void addTempoCommands (CommandRegistry&, te::SelectionManager&);

TimeSignature getTimeSignature (const te::Edit&);

// Changes the time signature, as one undo step, "Change Time Signature". An invalid one changes nothing.
void setTimeSignature (te::Edit&, TimeSignature);
} // namespace phazite
