// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's tempo and time signature (docs/design.md, section 4; roadmap step 1.7), which the top bar shows and
// changes, and the tempo's changes along the song, which the playlist's tempo lane shows and changes: each tempo from a
// beat on, held until the next or ramping to it, straight or bent (TempoChange, TempoValues.h). For now a song has one time
// signature, from its start. Clips stay on their beats when the tempo changes, as the playlist's time line is in beats:
// those that follow the tempo stretch with it, along its changes too, the others keep their speed (TempoStretch,
// TempoValues.h). The song holds them in its tempo map, a value beside its tree (getTempoMapValue()), which Phazite's engine
// plays (getTempoMap()).
#pragma once

#include "engine/core/TempoMap.h"
#include "model/base/Beats.h"
#include "model/base/Song.h"
#include "model/base/internal/Selection.h"
#include "model/base/internal/TempoMapValue.h"
#include "model/transport/TempoValues.h"

#include <optional>
#include <vector>

namespace phazite
{
class CommandRegistry;

//==============================================================================
// The song's tempo map as a value (model/base/internal/TempoMapValue.h), what the readers below read it from: each kind by
// beat, at least one of each. The song holds it in an area of its values (SongValues.h), which this refers to, valid until
// its next change; the edits below compute the next value from it and set it once (SongValues::setTempoMap()). A song that
// holds no values (one made to be read or checked: Song::getValues()) reads as makeDefaultTempoMap()'s.
const TempoMapValue& getTempoMapValue (const Song&);

// The song's tempo map (TempoValues.h), and its bars.
std::vector<TempoChange> getTempoChanges (const Song&);
std::vector<TimeSignatureChange> getTimeSignatureChanges (const Song&);
Bars getBars (const Song&);

// The song's tempo map as Phazite's engine plays it (engine/core/TempoMap.h), at a sample rate: at one sample a second, its
// beats in seconds, which audio clips, fades, recordings and renders count in. The song keeps the map made of its value for
// the last two sample rates asked, so that it is made once for each change of the tempo map (SongValues.h: PlayedTempoMap),
// and the readers below read it there; this returns a copy. And the same of a tempo map's value, and of a song's state,
// which needn't be open (a version's: model/versions/), read with readTempoMapValue().
engine::TempoMap getTempoMap (const Song&, double sampleRate = 1.0);
engine::TempoMap getTempoMap (const TempoMapValue&, double sampleRate);
engine::TempoMap getTempoMap (const juce::ValueTree& songState, double sampleRate);

// Where a beat is in seconds from the song's start, and the other way round; and its tempo at a beat, in beats per minute.
double toSeconds (const Song&, BeatPosition);
BeatPosition toBeats (const Song&, double seconds);
double getTempoAt (const Song&, BeatPosition);

// What every project's tempo map holds when it has none yet: a tempo of 120 BPM and 4/4 from the song's start, as a new
// song is, written into its tree's TEMPOMAP before the song holds its values (model/project/internal/Project.h: makeSong(),
// makeNewSong()). Not an undo step. Nothing for a song that holds its values, whose map is whole in its area.
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

// Changes the tempo where a beat is, as the top bar's field does where the playhead is: the tempo change in effect there
// (the last at or before it), as one undo step, "Change Tempo". With one tempo, that is setTempo().
void setTempoAt (Song&, BeatPosition, double bpm);

//==============================================================================
// The tempo's changes along the song, as the tempo lane changes them (model/transport/TempoModel.h): each by its place in
// getTempoChanges(), from 0, the first at the song's start. Each is a change of its own, one undo step named for it, which
// a drag's step takes in while one is open. Tempos stay within the limits, to a hundredth, and curves within -1 and 1.

// Whether the song has more than one tempo, which shows the tempo lane.
bool hasTempoChanges (const Song&);

// The tempo at evenly spaced beats over a range, from its start to its end (at least two), or at each of some beats, as
// the engine plays it.
std::vector<double> getTempoValues (const Song&, BeatRange, int count);
std::vector<double> getTempoValues (const Song&, const std::vector<BeatPosition>&);

// A tempo from a beat, "Add Tempo Change": on the tempo's line there, so that nothing changes until it moves, unless a bpm
// is given; its line to the next goes as the one it was on did (a step, unless it was on a ramp). A tempo already at that
// beat takes the bpm instead. Returns its place.
int addTempoChange (Song&, BeatPosition, std::optional<double> bpm = std::nullopt);

// Adds a tempo change where the playhead is, at the tempo there, as Add Tempo Change does.
void addTempoChangeAtPlayhead (Song&);

// Moves a tempo to a beat and a bpm, "Move Tempo Change" (the first stays at the song's start); and sets how its line to
// the next goes, "Change Tempo Ramp" (TempoChange::curve). Returns where it is after the move, or -1 for none.
int moveTempoChange (Song&, int index, BeatPosition, double bpm);
void setTempoCurve (Song&, int index, float curve);

// Deletes a tempo, "Delete Tempo Change" (never the first); and every tempo but the first, "Remove Tempo Changes", which
// leaves the song at its first tempo throughout.
void deleteTempoChange (Song&, int index);
void removeTempoChanges (Song&);

// Puts tempos into the song from a beat on, as a file brings them (a MIDI file's, a DAWproject's), each with its line to the
// next: one at the song's start is its first tempo, and before the first of them the song's tempo goes on. Each is a change
// of its own, which the caller's undo step takes in while one is open.
void takeTempoChanges (Song&, const std::vector<TempoChange>&, BeatPosition from);

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

// Gives the song these time signatures along it, each from its beat (in any order: they are put in order by beat), as a
// change of its own, one undo step, "Change Time Signatures". The model offers one time signature, from the song's start, to
// the user (setTimeSignature(), above); the song's tempo map and Phazite's engine take several, which tests of what plays give
// a song through this. Nothing changes for none, for one that isn't valid (isValid()) or starts before the song, for a list
// whose first doesn't start at the song's start, or for the time signatures the song holds already. Two given at one beat are
// both kept, in the order given.
void setTimeSignatureChanges (Song&, const std::vector<TimeSignatureChange>&);
} // namespace phazite
