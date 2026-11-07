// SPDX-License-Identifier: AGPL-3.0-or-later
// The chord lane's insides (ChordModel.h): its chords and keys in the song (Schema.h: CHORDLANE; docs/design.md, section
// 14). Edits use the song's undo manager and begin no step: their caller's holds them.
#pragma once

#include "model/base/Song.h"
#include "model/chords/ChordValues.h"

namespace phazite
{
struct Progression;

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

// The lane's state, added if it has none yet, with a key from 0 (the one getLaneKeys() gives until then).
juce::ValueTree getLaneState (Song&);

//==============================================================================
// Time edits (model/arrangement/internal/Arrangement.h): chords go with the song's time, as sections do. A chord that an
// edit cuts into gets shorter or longer.
void insertLaneTime (Song&, BeatPosition at, BeatDuration);
void removeLaneTime (Song&, BeatRange);

// The chords in a range, as moveTime() moves what is in it: those inside go to the destination, those between move the
// other way.
void moveLaneTime (Song&, BeatRange, BeatPosition destination);

// Copies of the chords in a range, cut at its edges, as copyTime() copies what is in it: taken before it makes room at the
// destination, and put there after it.
struct LaneCopy
{
    std::vector<juce::ValueTree> chords;
    BeatPosition rangeStart {};
};

LaneCopy copyLaneChords (const Song&, BeatRange);
void pasteLaneChords (Song&, const LaneCopy&, BeatPosition destination);
} // namespace phazite
