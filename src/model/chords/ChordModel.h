// SPDX-License-Identifier: AGPL-3.0-or-later
// The chord lane as the UI reaches it (docs/design.md, section 14; roadmap step 2.7), by plain values: the song's chords,
// each a numeral in the key there, which the playlist shows under its sections; the keys they are in; progressions laid
// over a section, a range or the whole song; and parts (chords, a bass line or an arpeggio) made from them, which change
// with the chords while their notes are the ones the lane made for them. Each edit is one named undo step. A chord is
// named by its start, since chords don't overlap. No Tracktion here (internal/Chords.h is its Tracktion side).
#pragma once

#include "model/base/IDs.h"
#include "model/chords/ChordValues.h"

#include <optional>
#include <vector>

namespace phazite
{
class CommandRegistry;
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
// Edits, each one undo step, which parts that follow the chords follow in the same step.

// "Apply Chord Progression": the progression over a range, from its start, repeated until the range is full, in place of
// the chords there.
void applyProgression (ProjectContext&, const Progression&, BeatRange);

// "Add Chord", in place of the chords in the range.
void addLaneChord (ProjectContext&, BeatRange, const Numeral&);

// "Change Chord": the chord that starts at a beat becomes another, in the same place.
void changeLaneChord (ProjectContext&, BeatPosition chordStart, const Numeral&);

// "Delete Chord".
void deleteLaneChord (ProjectContext&, BeatPosition chordStart);

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
// Where a progression or a part goes.

// The selected sections, from the first's start to the last's end; the time range selected across tracks; or the whole
// song, from its start to the end of its last clip or section, which is nothing for an empty song.
std::optional<BeatRange> getSelectedSectionsRange (const ProjectContext&);
std::optional<BeatRange> getSelectedTimeRange (const ProjectContext&);
std::optional<BeatRange> getWholeSongRange (const ProjectContext&);

// Where a part made from the chords goes: the time range selected, or else the selected sections, or else from the
// lane's first chord to the end of its last. Nothing without chords there.
std::optional<BeatRange> getChordPartRange (const ProjectContext&);

// How many beats a progression lasts from a beat, in the time signature there.
BeatDuration getProgressionLength (const ProjectContext&, const Progression&, BeatPosition from);

//==============================================================================
// Parts.

// A step of its own, "Make Chord Part", "Make Bass Line" or "Make Arpeggio": a new track at the end of the song with
// Phazite's synth set up for the part, and a clip on it over the range with the part's notes, which follow the chords.
// The track is selected. Returns the clip, or nothing for an empty range.
std::optional<ClipID> makeChordPart (ProjectContext&, PartStyle, BeatRange);
juce::String getMakePartName (PartKind); // "Make Chord Part"...

// The same where getChordPartRange() says, as the commands make parts; and whether there are chords there to make one.
std::optional<ClipID> makeChordPartHere (ProjectContext&, PartStyle);
bool canMakeChordPart (const ProjectContext&);

// The style a clip was made in from the chord lane, if it was; and whether its notes still follow the chords (they stop
// when they are edited).
std::optional<PartStyle> getChordPart (const ProjectContext&, ClipID);
bool followsChords (const ProjectContext&, ClipID);

// A step of its own: a part follows the chords again, its notes made again from them, "Follow the Chords"; or it becomes a
// clip like any other, "Stop Following the Chords".
void setFollowsChords (ProjectContext&, ClipID, bool);

//==============================================================================
// Hearing a progression before it goes into the song: its chords as they would play from a beat, written to a MIDI file
// for the previewer (model/engine/Preview.h), held chords in the key there. Returns false if the file can't be written.
bool writeProgressionPreview (const ProjectContext&, const Progression&, BeatPosition from, const juce::File&);

// The chord commands: the parts made from the chords over where getChordPartRange() says, in their first styles.
void addChordCommands (CommandRegistry&, ProjectContext&);
} // namespace phazite
