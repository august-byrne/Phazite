// SPDX-License-Identifier: AGPL-3.0-or-later
// Notes as plain values, as the piano roll shows them and the transforms change them, with keys and scales, the chords
// the piano roll stamps, and drums' names (docs/design.md, section 7). Positions are song beats (a clip's content start
// plus the note's own beat), so the notes of several clips line up. The transforms (quantize, legato, chop, transpose,
// strum, humanize, arpeggiate) work on copies of the notes, so that the piano roll can show what one would do before it
// is applied. Notes.h holds them in the song.
#pragma once

#include "model/base/Beats.h"
#include "model/base/ControlKinds.h"
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

    // As FL Studio's: a slide note plays nothing itself, but glides the notes sounding as it starts to its pitch over its
    // length (an 808's glide); a portamento note glides in from the note played before it. Phazite's own instruments play
    // them; plug-ins get no slide notes, and portamento notes as plain ones.
    bool slide = false, portamento = false;

    // The MIDI channel it plays on, from 1 to 16, as musicians count them: which part of a plug-in with several parts plays
    // it. A new note's is 1; Phazite's own instruments play every channel alike.
    int channel = 1;

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

// A length as the piano roll's toolbar and menus show it, in beats as the grid names its steps: "1/4 beat", "3/4 beat",
// "1 beat", "2 beats", "1.5 beats".
juce::String describeNoteLength (BeatDuration);

//==============================================================================
// Controllers: what a MIDI clip holds beside its notes (Notes.h: addControl()), as MIDI has them, their kinds
// (ControlKind, hasNumber(), getHighestValue()) in model/base/ControlKinds.h.

// A controller's name, as the piano roll's lanes name it: "Sustain Pedal", "Mod Wheel", "Brightness" (JUCE's names, tidied),
// or nothing for a number that has none.
juce::String getControllerName (int number);

// What the piano roll's controller lane shows (docs/design/piano-roll.md, "Controller lanes"): one kind of value on one MIDI
// channel, and for a controller, which one. A note's pressure is every note's: its lane has no number. Values are steps, as
// MIDI has them: each holds until the next.
struct ControlLane
{
    ControlKind kind = ControlKind::controller;
    int number = 0;  // a controller's, 0 to 127; 0 for the other kinds
    int channel = 1; // 1 to 16

    // "Sustain Pedal (CC 64)", "Brightness (CC 74)", "CC 21", "Pitch Bend", "Channel Pressure", "Note Pressure", "Program
    // Change", with ", Channel 2" after it on another channel than the first. And without its number and channel, for the
    // undo steps that change it ("Draw Sustain Pedal"): "Sustain Pedal", "CC 21", "Pitch Bend".
    juce::String getName() const;
    juce::String getShortName() const;

    int getHighestValue() const noexcept; // 16383 for the pitch bend, 127 for the others
    int getRest() const noexcept;         // where it rests: the bend's middle, 8192, and 0 for the others
    bool canDraw() const noexcept;        // all but a note's pressure, which is shown and deleted

    // A value as the hint bar says it: "Sustain Pedal 127", "Pitch Bend -50%" (from -100% to 100%), "Program 12" (programs
    // counted from 1, as Ableton Live shows them).
    juce::String describeValue (int value) const;

    bool operator== (const ControlLane&) const = default;
};

// In the order the piano roll's lane menu lists them: by channel, then kind, then number.
bool operator< (const ControlLane&, const ControlLane&) noexcept;

// The lane of a kind of value, its number kept only where the kind has one and both kept in their ranges.
ControlLane makeControlLane (ControlKind, int number, int channel);

// The lanes the menu offers whatever the clips hold, on the first channel: Pitch Bend, Mod Wheel (CC 1), Sustain Pedal
// (CC 64), Expression (CC 11), Volume (CC 7), Pan (CC 10), Channel Pressure, Program Change.
std::vector<ControlLane> getUsualControlLanes();

// A value of a lane where it is in the song: in a clip (the clip it is to go in, for one still to be drawn), at a song beat;
// and for a note's pressure, the note's number.
struct ControlPoint
{
    ClipID clip {};
    BeatPosition beat {};
    int value = 0;
    int number = 0;
};

// Values are no closer together than this (1/64 of a beat): a drawn line or a ramp makes one at each step of it at most.
BeatDuration getShortestControlGap();

// The values a straight ramp from one value to another is made of, from the earlier beat to the later, both included: one
// at each getShortestControlGap() at most, each kept within the lane's range, one that repeats the value before it left
// out, as a recording leaves it out. A ramp of no length is the value at its end alone.
std::vector<ControlPoint> makeControlRamp (const ControlLane&, BeatPosition from, int fromValue, BeatPosition to, int toValue);

//==============================================================================
// Transforms: what a change would make of some notes. The notes of a chord are those of one clip that start together
// (within a 128th of a beat).

struct QuantizeSettings
{
    double gridBeats = 0.25; // the step of the grid to quantize to
    float strength = 1.0f;   // how far toward the grid line each note moves, from 0 to 1
    float swing = 0.0f;      // how late every other grid line is, from 0 to 1, where 1 is a triplet feel
    bool ends = false;       // the ends too, not only the starts
    BeatPosition origin {};  // where the grid's lines count from: the piano roll's bar 1, the start of its clip
};

std::vector<NoteData> quantize (std::vector<NoteData>, const QuantizeSettings&);

// Each note lasts until the next starts (notes that start together are a chord and end together), less a
// gap. Notes with nothing after them keep their length.
std::vector<NoteData> legato (std::vector<NoteData>, double gapBeats = 0.0);

// The Legato command's name with the gap it leaves: "Legato", or "Legato (1/16 beat gap)".
juce::String describeLegato (double gapBeats);

// Cuts each note into pieces of a length; the last piece is shorter if the note doesn't divide evenly.
std::vector<NoteData> chop (std::vector<NoteData>, double pieceBeats);

// Strums each chord, as a guitar's strings sound one after another: its notes start a step apart, from the lowest up or
// from the highest down, and each keeps its end. Single notes stay as they are.
struct StrumSettings
{
    double stepBeats = 1.0 / 32.0; // how much later each note of a chord starts than the one before it
    bool down = false;             // from the highest note down, rather than from the lowest up
};

std::vector<NoteData> strum (std::vector<NoteData>, const StrumSettings&);

// Moves each note's start earlier or later, and its velocity up or down, by a random amount up to a limit, as a player's
// timing and touch vary. The same settings give the same result, so that what the piano roll previews is what it applies.
struct HumanizeSettings
{
    double timingBeats = 1.0 / 64.0; // the most a note's start moves, either way
    int velocity = 10;               // the most its velocity changes, either way
    int seed = 1;
};

std::vector<NoteData> humanize (std::vector<NoteData>, const HumanizeSettings&);

// Plays each chord's notes one after another instead of together, a step apart, for as long as the chord lasts (until its
// longest note ends), each note as loud as it was. Single notes stay as they are.
enum class ArpeggioOrder
{
    up,       // from the lowest note
    down,     // from the highest
    upAndDown // up, then down again
};

struct ArpeggioSettings
{
    double stepBeats = 0.25; // how far apart the notes start: the piano roll gives the grid's step
    ArpeggioOrder order = ArpeggioOrder::up;
    float gate = 1.0f; // how long each note lasts, as a part of the step, from 0.1 to 1
};

std::vector<NoteData> arpeggiate (std::vector<NoteData>, const ArpeggioSettings&);

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
juce::String describeKey (const Key&);    // "C major", "Bb major", "F# minor": its root as getKeyRootLetter() spells it

// Notes spelled by letter, as the key's name and the chords' names spell them (chords/ChordValues.h). The major key with a
// key's notes: its parent, whose scale the key's is a mode of (C major for D dorian or A minor; harmonic and melodic minor
// count as minor). The letter a key's root is written with (0 for C to 6 for B), as its parent major's signature has it,
// with flats for F, Bb, Eb, Ab and Db major. And a pitch class written with a letter (any number, taken round the seven),
// with as many sharps or flats as it takes: "Eb", "D#", "Cb".
int getParentMajor (const Key&);
int getKeyRootLetter (const Key&);
juce::String spellWithLetter (int letter, int pitchClass);
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
