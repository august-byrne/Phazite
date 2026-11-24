// SPDX-License-Identifier: AGPL-3.0-or-later
// The chord lane's insides (ChordModel.h): its chords and keys in the song (Schema.h: CHORDLANE; docs/design.md, section
// 14). The song holds its lane as a value in an area of its values (model/base/internal/SongValues.h, ChordLaneArea.h): every
// edit below reads the value, works the next one out and sets it once (SongValues::setChordLane()), in the step in progress,
// or outside the history while the song is written outside it; none begins a step: their caller's holds them. A chord's and
// a key change's entries have no identity of their own (they are found by where they start, two within a ten-thousandth of a
// beat as one), so the lane is never written into elements by what differs.
#pragma once

#include "model/base/Song.h"
#include "model/base/internal/ChordLaneValue.h"
#include "model/chords/ChordValues.h"

#include <juce_data_structures/juce_data_structures.h>

#include <vector>

namespace phazite
{
struct Progression;

// The song's chord lane as a value (model/base/internal/ChordLaneValue.h): its chords and keys, each kind in the song's
// order. The area's value, cheap to copy; for a song that keeps everything in its tree (one made to be read or checked,
// without values: Song::getValues()), read from its CHORDLANE element (readChordLaneValue()). Empty for a song without a lane.
ChordLaneValue getChordLaneValue (const Song&);

// Whether the song has a chord lane: one is added with its first chord or key, starting with the key new tracks get at that
// moment (the edits below); until then its keys are that key (getLaneKeys()). The lane's value says so (ChordLaneValue.h:
// placed), which the writers below set in the step that adds or takes away its element in the song's tree (Schema.h:
// CHORDLANE, an empty placeholder beside it); for a song without values, that element.
bool hasChordLane (const Song&);

// The song's lane made the one an element holds (a CHORDLANE, read as the song holds one: ChordLaneValue.h,
// readHeldChordLane()), or none for an invalid tree: its element in the song's tree added or taken out where it has one and
// the other none, and its value set, in the mode of the moment (the step in progress, or outside the history while the song
// is written outside it). What gives the song a version's lane and a merge's choice of one (model/versions/ApplySong.cpp,
// chooseChordLaneClash()). Whether it wrote anything.
bool makeChordLaneAs (Song&, const juce::ValueTree& chordLaneOrNone);

// The lane's chords, by their starts, each with the key it is in.
std::vector<PlacedChord> getLaneChords (const Song&);

// The lane's keys, by their starts: at least one, from 0. Before anything is written, the key new tracks get (the new
// project's), or else C major.
struct LaneKey
{
    BeatPosition start {};
    Key key {};
};

std::vector<LaneKey> getLaneKeys (const Song&);
Key getLaneKeyAt (const Song&, BeatPosition);

//==============================================================================
// Edits of the lane.

// Makes room for chords in a range: those inside it go, and those that reach into it are cut at its edges.
void clearLaneRange (Song&, BeatRange);

// Puts a chord over a range, in place of what was there.
void putLaneChord (Song&, BeatRange, const Numeral&);

// The chord that starts at a beat: whether there is one; another in its place; gone; or ending at another beat, over
// the chords after it if it grows.
bool hasLaneChordAt (const Song&, BeatPosition start);
void setLaneChordNumeral (Song&, BeatPosition start, const Numeral&);
void removeLaneChord (Song&, BeatPosition start);
void setLaneChordEnd (Song&, BeatPosition start, BeatPosition end);

// The key from a beat to the next key change: the key change at the beat, or a new one there. And a key change gone (not
// the one at 0).
void setLaneKey (Song&, BeatPosition from, const Key&);
void removeLaneKeyChange (Song&, BeatPosition at);

// Lays a progression over a range from its start, again and again until the range is full (cutting the last chord that
// doesn't fit), in place of what was there. Its chords become numerals in the lane's key there: a minor progression in a
// major key plays in its relative minor (getKeyForProgression()).
void putProgression (Song&, const Progression&, BeatRange);

//==============================================================================
// Time edits (model/arrangement/internal/Arrangement.h): chords go with the song's time, as sections do. A chord that an
// edit cuts into gets shorter or longer.
void insertLaneTime (Song&, BeatPosition at, BeatDuration);
void removeLaneTime (Song&, BeatRange);

// The chords in a range, as moveTime() moves what is in it: those inside go to the destination, those between move the
// other way.
void moveLaneTime (Song&, BeatRange, BeatPosition destination);

// Copies of the chords in a range, cut at its edges, as copyTime() copies what is in it: taken before it makes room at the
// destination, and put there after it. Values, as the lane holds them.
struct LaneCopy
{
    std::vector<ChordEntry> chords;
    BeatPosition rangeStart {};
};

LaneCopy copyLaneChords (const Song&, BeatRange);
void pasteLaneChords (Song&, const LaneCopy&, BeatPosition destination);
} // namespace phazite
