// SPDX-License-Identifier: AGPL-3.0-or-later
// Notes as plain values (architecture phase A3: no Tracktion here), as the piano roll shows them and the transforms change
// them, with keys and scales, the chords the piano roll stamps, and drums' names (docs/design.md, section 7). Positions
// are song beats (a clip's content start plus the note's own beat), so the notes of several clips line up. The transforms
// (quantize, legato, chop, transpose) work on copies of the notes, so that the piano roll can show what one would do before
// it is applied. Notes.h holds their Tracktion side.
#pragma once

#include "model/base/Beats.h"
#include "model/base/IDs.h"

#include <juce_core/juce_core.h>

#include <optional>
#include <vector>

namespace phazite
{
// A note as the piano roll shows it and the transforms change it.
struct NoteData
{
    ClipID clip {}; // the clip it is in, or is to go in
    NoteID id {};   // which of the clip's notes it is: none for one still to be added, such as a transform's
    int pitch = 60;
    BeatPosition start {}; // in the song
    BeatDuration length = BeatDuration::fromBeats (1.0);
    int velocity = 100;
    bool muted = false;

    NoteRef getRef() const noexcept { return { clip, id }; }
    BeatPosition getEnd() const noexcept { return start + length; }
};

// The notes' references, in their order.
inline std::vector<NoteRef> getRefs (const std::vector<NoteData>& notes)
{
    std::vector<NoteRef> refs;

    for (const auto& note : notes)
        refs.push_back (note.getRef());

    return refs;
}

// Notes are at least this long.
BeatDuration getMinimumNoteLength();

//==============================================================================
// Transforms: what a change would make of some notes.

struct QuantizeSettings
{
    double gridBeats = 0.25; // the step of the grid to quantize to
    float strength = 1.0f;   // how far toward the grid line each note moves, from 0 to 1
    float swing = 0.0f;      // how late every other grid line is, from 0 to 1, where 1 is a triplet feel
    bool ends = false;       // the ends too, not only the starts
};

std::vector<NoteData> quantize (std::vector<NoteData>, const QuantizeSettings&);

// Each note lasts until the next starts (notes that start together are a chord and end together), less a
// gap. Notes with nothing after them keep their length.
std::vector<NoteData> legato (std::vector<NoteData>, double gapBeats = 0.0);

// Cuts each note into pieces of a length; the last piece is shorter if the note doesn't divide evenly.
std::vector<NoteData> chop (std::vector<NoteData>, double pieceBeats);

//==============================================================================
// Keys and scales.

// The scales Phazite offers for a key, in the order its menus show them.
enum class Scale
{
    major,
    minor,
    dorian,
    phrygian,
    lydian,
    mixolydian,
    locrian,
    harmonicMinor,
    melodicMinor
};

struct Key
{
    int root = 0; // 0 is C, 1 C sharp, ..., 11 B
    Scale scale = Scale::major;

    bool operator== (const Key&) const = default;
};

const std::vector<Scale>& getKeyScales(); // every scale, in the order menus show them
juce::String getScaleName (Scale);        // "Major", "Harmonic Minor"
juce::String describeKey (const Key&);    // "C major"
bool isInKey (int pitch, const Key&);

// The semitones of a scale's seven steps above its root, from 0: { 0, 2, 4, 5, 7, 9, 11 } for major.
std::vector<int> getScaleSteps (Scale);

// A scale as projects save it ("major", "dorian", "harmonicMinor"...), and back: nothing for a name that isn't one of
// them.
juce::String getSavedScaleName (Scale);
std::optional<Scale> findScale (const juce::String& savedName);

// The pitch in the key nearest to a pitch (the lower one when two are as near).
int snapToKey (int pitch, const Key&);

// Transposes by semitones, or with a key, by steps of its scale (a note outside the scale first goes to the
// nearest one in it). Pitches stay within the MIDI range.
std::vector<NoteData> transpose (std::vector<NoteData>, int steps, const std::optional<Key>& diatonicIn);

//==============================================================================
// Chords: the kinds the piano roll stamps with one click, and the chord lane's chords have (model/chords/ChordValues.h).

enum class ChordType
{
    major,
    minor,
    dominantSeventh,
    majorSeventh,
    minorSeventh,
    suspendedSecond,
    suspendedFourth,
    diminished,
    augmented,
    majorSixth,
    minorSixth,
    dominantNinth,
    majorNinth,
    power,
    halfDiminishedSeventh, // m7b5
    diminishedSeventh,
    minorNinth
};

struct ChordShape
{
    const char* name; // "Major", "m7"
    ChordType type;
};

const std::vector<ChordShape>& getChordShapes();
std::vector<int> getChordIntervals (ChordType);         // semitones above the root, from 0: { 0, 4, 7 } for major
std::vector<int> getChordPitches (int root, ChordType); // those in the MIDI range

//==============================================================================
// Drum mode: rows named after the drums instead of piano keys.

struct DrumRow
{
    int note = 36;
    juce::String name;
};

// General MIDI's drum for a note ("Kick", "Snare", "Closed Hat"...), or the note's name outside that map.
juce::String getDrumName (int note);
} // namespace phazite
