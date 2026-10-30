// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's tempo and time signature (docs/design.md, section 4; roadmap step 1.7), which the top bar shows and
// changes. For now a song has one of each, from its start; changes along the song come later, with a tempo lane. Clips
// stay on their beats when the tempo changes, as the playlist's time line is in beats: those that follow the tempo
// stretch with it, the others keep their speed (TempoStretch, TempoValues.h). The song holds them in its tempo map,
// Phazite's own (schema::TEMPOMAP; docs/tracktion-migration.md, T1), which Tracktion's tempo sequence mirrors.
#pragma once

#include "model/base/internal/Selection.h"
#include "model/base/internal/Tracktion.h"
#include "model/transport/TempoValues.h"

#include <vector>

namespace phazite
{
class CommandRegistry;

//==============================================================================
// The tempo map: each tempo and time signature from a beat on, in the song's order, the first at its start. A song
// without one is at 120 BPM in 4/4.
struct TempoChange
{
    BeatPosition beat {};
    double bpm = 120.0;
};

struct TimeSignatureChange
{
    BeatPosition beat {};
    TimeSignature signature {};
};

std::vector<TempoChange> getTempoChanges (const te::Edit&);
std::vector<TimeSignatureChange> getTimeSignatureChanges (const te::Edit&);

// The time signature of the bar that is at a beat.
TimeSignature getTimeSignatureAt (const te::Edit&, BeatPosition);

// What every project's tempo map holds when it has none yet: a tempo of 120 BPM and 4/4 from the song's start, as a new
// song is. Not an undo step: Project adds it as a project opens.
void ensureTempoMap (te::Edit&);

// The time edits (model/arrangement/internal/Arrangement.h): the tempos and time signatures after a beat move later by a
// length; and those in a range go, the ones after it moving earlier by its length. The first, at the song's start, stays.
void insertTempoMapTime (te::Edit&, BeatPosition, BeatDuration);
void removeTempoMapTime (te::Edit&, BeatRange);

//==============================================================================
// The song's tempo: the first in its tempo map.
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
void addTempoCommands (CommandRegistry&, Selection&);

// The song's time signature: the first in its tempo map.
TimeSignature getTimeSignature (const te::Edit&);

// Changes the time signature, as one undo step, "Change Time Signature". An invalid one changes nothing.
void setTimeSignature (te::Edit&, TimeSignature);
} // namespace phazite
