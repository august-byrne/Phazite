// SPDX-License-Identifier: AGPL-3.0-or-later
// Chords as plain values (docs/design.md, section 14; roadmap step 2.7): Phazite's own chord format, a Roman numeral
// relative to a key, so that a chord follows the key when it changes; what a numeral is in a key (its root, notes and
// name); and chords written as notes.
#pragma once

#include "model/notes/NoteValues.h"

#include <optional>
#include <vector>

namespace phazite
{
//==============================================================================
// A chord relative to a key: its root on a step of the key's scale, raised or lowered by a semitone ("bVII" in a major
// key is a whole step under its tonic), with its kind and inversion.
struct Numeral
{
    int degree = 1;     // the step of the key's scale, 1 to 7
    int accidental = 0; // -1 (flat), 0 or 1 (sharp)
    ChordType type = ChordType::major;
    int inversion = 0; // 0: the root lowest; 1: the next note up lowest; and so on

    bool operator== (const Numeral&) const = default;
};

// Reads a numeral as Phazite's progressions write it: an accidental ("b" or "#"); the step in Roman numerals, in upper
// case for a chord with a major third and lower case for one with a minor third ("IV", "vi"); then its kind: nothing
// (a triad), "7", "maj7", "6", "9", "maj9", "sus2", "sus4", "5", "o" or "dim", "+" or "aug", "o7" or "dim7", and
// "m7b5"; and an inversion, "/1" to "/3". So "ii7" is a minor seventh, "V7" a dominant seventh and "bVII" a major
// chord on the lowered seventh step. Nothing if the text isn't a numeral.
std::optional<Numeral> parseNumeral (const juce::String&);

// How Phazite writes a numeral: "bVII", "ii7", "IVmaj7", "V/1", with a degree sign for diminished chords ("vii" and the
// sign) and a slashed o for half-diminished ones.
juce::String describeNumeral (const Numeral&);

// What a numeral is in a key: its root (0 to 11, 0 is C), its notes' pitch classes (from the root up, in its inversion's
// order), and its name ("Bb", "Am7", "F#dim", "Gsus4", "C/E"), each note spelled with the letter of its step of the
// scale (the leading note of D harmonic minor is C#, its sixth Bb).
int getRootIn (const Numeral&, const Key&);
std::vector<int> getPitchClassesIn (const Numeral&, const Key&);
juce::String getChordNameIn (const Numeral&, const Key&);

// A chord's symbol after its root: "", "m", "7", "maj7", "m7", "sus2", "sus4", "dim", "aug", "6", "m6", "9", "maj9", "5",
// "m7b5", "dim7", "m9".
juce::String getChordSymbol (ChordType);

// The numeral of a chord with a root (0 to 11) and kind in a key: on the scale's step with that note, or else on the
// step below it, raised, or the one above it, lowered (as a lowered seventh, "bVII", is written in a major key).
Numeral toNumeral (int root, ChordType, int inversion, const Key&);

// The seven chords of a key: on each step of its scale, the triad its notes make.
std::vector<Numeral> getDiatonicChords (const Key&);

// Whether a key's third is major (major, lydian, mixolydian) rather than minor.
bool hasMajorThird (const Key&);

// The key a progression written for major or minor plays in, in a song in a key: the song's key's root in the
// progression's scale when both have the same third, or else the major or minor key with the song's key's notes (A
// minor's i-VI-III-VII in C major plays Am-F-C-G; a major progression in D dorian plays in C major).
Key getKeyForProgression (const Key& songKey, Scale progressionScale);

// A chord kind as projects save it ("major", "dominantSeventh"...), and back: nothing for a name that isn't one.
juce::String getSavedChordTypeName (ChordType);
std::optional<ChordType> findChordType (const juce::String& savedName);

//==============================================================================
// Chords written as notes, voiced and played in a style: for hearing a progression before it goes into the song (held
// triads over their roots), and for the example songs' parts.
enum class PartStyle
{
    sustainedTriads, // chords, each held for its length
    spreadChords,    // open: the root low, the other notes spread above it
    offbeatStabs,    // short chords between the beats
    bassRoots        // the root, held
};

// A chord over a stretch of the song, with the key it is in there.
struct PlacedChord
{
    BeatRange range {};
    Numeral numeral {};
    Key key {};
};

// The notes of chords in a style, from the range's start to its end, in song beats, to go in a clip: in a register that suits
// the style (chords around middle C, roots an octave and a half under it), each chord voiced to move as little as it can from
// the one before. Notes don't reach past their chord's end or the range's.
std::vector<NoteData> makePartNotes (const std::vector<PlacedChord>&, PartStyle, BeatRange, ClipID);
} // namespace phazite
