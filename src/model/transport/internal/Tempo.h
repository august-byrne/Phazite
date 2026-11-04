// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's tempo and time signature (docs/design.md, section 4; roadmap step 1.7), which the top bar shows and
// changes. For now a song has one of each, from its start; changes along the song come later, with a tempo lane. Clips
// stay on their beats when the tempo changes, as the playlist's time line is in beats: those that follow the tempo
// stretch with it, the others keep their speed (TempoStretch, TempoValues.h). The song holds them in its tempo map
// (schema::TEMPOMAP), which Phazite's engine plays (getTempoMap()).
#pragma once

#include "engine/core/TempoMap.h"
#include "model/base/Beats.h"
#include "model/base/Song.h"
#include "model/base/internal/Selection.h"
#include "model/transport/TempoValues.h"

#include <vector>

namespace phazite
{
class CommandRegistry;

//==============================================================================
// The song's tempo map (TempoValues.h), and its bars.
std::vector<TempoChange> getTempoChanges (const Song&);
std::vector<TimeSignatureChange> getTimeSignatureChanges (const Song&);
Bars getBars (const Song&);

// The song's tempo map as Phazite's engine plays it (engine/core/TempoMap.h), at a sample rate: at one sample a second, its
// beats in seconds, which audio clips, fades, recordings and renders count in.
engine::TempoMap getTempoMap (const Song&, double sampleRate = 1.0);

// Where a beat is in seconds from the song's start, and the other way round; and its tempo at a beat, in beats per minute.
double toSeconds (const Song&, BeatPosition);
BeatPosition toBeats (const Song&, double seconds);
double getTempoAt (const Song&, BeatPosition);

// What every project's tempo map holds when it has none yet: a tempo of 120 BPM and 4/4 from the song's start, as a new
// song is. Not an undo step: Project adds it as a project opens.
void ensureTempoMap (Song&);

// The time edits (model/arrangement/internal/Arrangement.h): the tempos and time signatures after a beat move later by a
// length; and those in a range go, the ones after it moving earlier by its length. The first, at the song's start, stays.
void insertTempoMapTime (Song&, BeatPosition, BeatDuration);
void removeTempoMapTime (Song&, BeatRange);

//==============================================================================
// The song's tempo: the first in its tempo map.
double getTempo (const Song&);

// Changes the tempo, as one undo step, "Change Tempo" (a drag's changes join the drag's step). It stays within the
// limits (TempoValues.h), to a hundredth of a beat per minute.
void setTempo (Song&, double bpm);

// What a change of the tempo stretches.
TempoStretch getTempoStretch (const Song&);

// The Samples Follow Tempo command's switch: on while every sample in the song follows the tempo (and there is one).
// Toggled, every sample follows it, or none does if all did (model/arrangement/internal/AudioClips.h).
bool doAllSamplesFollowTempo (const Song&);
void toggleSamplesFollowTempo (Song&);

// The Patterns Follow Tempo command's switch, the song's (schema::patternsKeepTheirSpeed): on, a tempo change leaves
// MIDI clips' notes on their beats, so they play faster or slower; off, it moves the notes to keep their times.
bool doPatternsFollowTempo (const Song&);
void togglePatternsFollowTempo (Song&);

// Registers the commands that say what a tempo change stretches.
void addTempoCommands (CommandRegistry&, Selection&);

// The song's time signature: the first in its tempo map.
TimeSignature getTimeSignature (const Song&);

// Changes the time signature, as one undo step, "Change Time Signature". An invalid one changes nothing.
void setTimeSignature (Song&, TimeSignature);
} // namespace phazite
