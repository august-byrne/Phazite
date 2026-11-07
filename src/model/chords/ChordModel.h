// SPDX-License-Identifier: AGPL-3.0-or-later
// The chord lane as the UI reaches it (docs/design.md, section 14; roadmap step 2.7), by plain values: the song's chords,
// each a numeral in the key there, which the playlist shows under its bar numbers while there are some; the keys they are
// in; and progressions laid over a section, a range or the whole song. Each edit is one named undo step. A chord is named
// by its start, since chords don't overlap. Its insides, in the song, are internal/Chords.h.
#pragma once

#include "model/base/IDs.h"
#include "model/chords/ChordValues.h"

#include <optional>
#include <vector>

namespace phazite
{
class ProjectContext;
struct Progression;

// A chord of the lane, with what the playlist and the piano roll show of it.
struct LaneChord
{
    BeatRange range {};
    Numeral numeral {};
    Key key {};                    // the lane's key there
    juce::String name;             // "Am7", "G/B"
    std::vector<int> pitchClasses; // its notes, 0 to 11, the lowest first
};

std::vector<LaneChord> getLaneChords (const ProjectContext&); // by their starts
std::optional<LaneChord> getLaneChordAt (const ProjectContext&, BeatPosition);
bool hasLaneChords (const ProjectContext&);

// The lane's keys, by their starts: at least one, from 0. Until a key is chosen, the new project's (the one new tracks
// get), or else C major.
struct LaneKeyChange
{
    BeatPosition start {};
    Key key {};
};

std::vector<LaneKeyChange> getLaneKeyChanges (const ProjectContext&);
Key getLaneKeyAt (const ProjectContext&, BeatPosition);

//==============================================================================
// Edits, each one undo step.

// "Apply Chord Progression": the progression over a range, from its start, repeated until the range is full, in place of
// the chords there.
void applyProgression (ProjectContext&, const Progression&, BeatRange);

// "Add Chord", in place of the chords in the range.
void addLaneChord (ProjectContext&, BeatRange, const Numeral&);

// "Change Chord": the chord that starts at a beat becomes another, in the same place.
void changeLaneChord (ProjectContext&, BeatPosition chordStart, const Numeral&);

// "Delete Chord".
void deleteLaneChord (ProjectContext&, BeatPosition chordStart);

// "Delete All Chords": every chord, and every key change after the song's start, which only the chords follow; the song's key
// stays. The playlist then shows no chord lane.
void deleteAllLaneChords (ProjectContext&);

// "Change Chord Length": the chord that starts at a beat ends at another, over the chords after it if it grows.
void setLaneChordEnd (ProjectContext&, BeatPosition chordStart, BeatPosition end);

// "Change Key": the key from a beat to the next key change, the key change at the beat or a new one there (from 0, the
// song's key). The chords keep their numerals, so they move with the key.
void setLaneKey (ProjectContext&, BeatPosition from, const Key&);

// "Remove Key Change": the key change at a beat goes, and the key before it goes on. The one at 0 stays.
void removeLaneKeyChange (ProjectContext&, BeatPosition at);

// The chords a key offers at a beat: the seven of its scale, then common ones from outside it (borrowed from the parallel
// key, and the dominants of the scale's chords), each once.
std::vector<Numeral> getChordChoices (const ProjectContext&, BeatPosition);

//==============================================================================
// Where a progression goes.

// The selected sections, from the first's start to the last's end; the time range selected across tracks; or the whole
// song, from its start to the end of its last clip or section, which is nothing for an empty song.
std::optional<BeatRange> getSelectedSectionsRange (const ProjectContext&);
std::optional<BeatRange> getSelectedTimeRange (const ProjectContext&);
std::optional<BeatRange> getWholeSongRange (const ProjectContext&);

// How many beats a progression lasts from a beat, in the time signature there.
BeatDuration getProgressionLength (const ProjectContext&, const Progression&, BeatPosition from);

//==============================================================================
// Hearing a progression before it goes into the song: its chords as they would play from a beat, written to a MIDI file
// for the previewer (model/engine/Preview.h), held chords in the key there. Returns false if the file can't be written.
bool writeProgressionPreview (const ProjectContext&, const Progression&, BeatPosition from, const juce::File&);
} // namespace phazite
