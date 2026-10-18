// SPDX-License-Identifier: AGPL-3.0-or-later
// The chord lane's Tracktion side (ChordModel.h): its chords and keys in the song's Phazite state (Schema.h: CHORDLANE),
// and the parts made from them, MIDI clips that follow the chords while their notes are the ones the lane makes for them
// (docs/design.md, section 14). Edits use the Edit's undo manager and begin no step: their caller's holds them.
#pragma once

#include "model/base/internal/Tracktion.h"
#include "model/chords/ChordValues.h"

#include <functional>

namespace phazite
{
struct Progression;

// The lane's chords, by their starts, each with the key it is in.
std::vector<PlacedChord> getLaneChords (const te::Edit&);

// The lane's keys, by their starts: at least one, from 0. Before anything is written, the key new tracks get (the new
// project's), or else C major.
struct LaneKey
{
    BeatPosition start {};
    Key key {};
};

std::vector<LaneKey> getLaneKeys (const te::Edit&);
Key getLaneKeyAt (const te::Edit&, BeatPosition);

//==============================================================================
// Edits of the lane. Each keeps the parts that follow the chords following them (changeLane()).

// Makes room for chords in a range: those inside it go, and those that reach into it are cut at its edges.
void clearLaneRange (te::Edit&, BeatRange);

// Puts a chord over a range, in place of what was there.
void putLaneChord (te::Edit&, BeatRange, const Numeral&);

// The chord that starts at a beat: whether there is one; another in its place; gone; or ending at another beat, over
// the chords after it if it grows.
bool hasLaneChordAt (const te::Edit&, BeatPosition start);
void setLaneChordNumeral (te::Edit&, BeatPosition start, const Numeral&);
void removeLaneChord (te::Edit&, BeatPosition start);
void setLaneChordEnd (te::Edit&, BeatPosition start, BeatPosition end);

// The key from a beat to the next key change: the key change at the beat, or a new one there. And a key change gone (not
// the one at 0).
void setLaneKey (te::Edit&, BeatPosition from, const Key&);
void removeLaneKeyChange (te::Edit&, BeatPosition at);

// Lays a progression over a range from its start, again and again until the range is full (cutting the last chord that
// doesn't fit), in place of what was there. Its chords become numerals in the lane's key there: a minor progression in a
// major key plays in its relative minor (getKeyForProgression()).
void putProgression (te::Edit&, const Progression&, BeatRange);

// Runs a change of the lane, then remakes the notes of the parts that followed the chords before it (their notes were the
// ones the lane made for them), so that they follow the chords as they are after it.
void changeLane (te::Edit&, const std::function<void()>& change);

// The lane's state, added if it has none yet, with a key from 0 (the one getLaneKeys() gives until then).
juce::ValueTree getLaneState (te::Edit&);

//==============================================================================
// Time edits (model/arrangement/internal/Arrangement.h): chords go with the song's time, as sections do. A chord that an
// edit cuts into gets shorter or longer; parts, which are clips, go with their tracks.
void insertLaneTime (te::Edit&, BeatPosition at, BeatDuration);
void removeLaneTime (te::Edit&, BeatRange);

// The chords in a range, as moveTime() moves what is in it: those inside go to the destination, those between move the
// other way.
void moveLaneTime (te::Edit&, BeatRange, BeatPosition destination);

// Copies of the chords in a range, cut at its edges, as copyTime() copies what is in it: taken before it makes room at the
// destination, and put there after it.
struct LaneCopy
{
    std::vector<juce::ValueTree> chords;
    BeatPosition rangeStart {};
};

LaneCopy copyLaneChords (const te::Edit&, BeatRange);
void pasteLaneChords (te::Edit&, const LaneCopy&, BeatPosition destination);

//==============================================================================
// Parts: MIDI clips whose notes the lane made (schema::phaziteChordPart).

std::optional<PartStyle> getChordPartStyle (const te::Clip&);

// The notes the lane makes for a part where it is: from the clip's start to its end.
std::vector<NoteData> makeChordPartNotes (const te::MidiClip&, PartStyle);

// Whether a part's notes are the ones the lane makes for it, so that they change with the chords.
bool followsChords (const te::MidiClip&);

// Makes a clip a part in a style, with the notes the lane makes for it; with no style, it is a clip like any other.
void setChordPart (te::MidiClip&, std::optional<PartStyle>);

// Adds a track at the end of the song with an instrument for the part, and the part on it over a range. Returns the clip.
te::MidiClip* addChordPart (te::Edit&, PartStyle, BeatRange, te::SelectionManager*);
} // namespace phazite
