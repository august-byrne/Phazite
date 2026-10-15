// SPDX-License-Identifier: AGPL-3.0-or-later
// Notes in MIDI clips, as the piano roll edits them (docs/design.md, section 7). Positions are song beats
// (the clip's content start plus the note's own beat), so the notes of several clips line up. The
// transforms (quantize, legato, chop, transpose) work on copies of the notes, so that the piano roll can
// show what one would do before it is applied with replaceNotes().
#pragma once

#include "model/base/Tracktion.h"

#include <functional>
#include <optional>
#include <vector>

namespace phazite
{
class CommandRegistry;

// A note as the piano roll shows it and the transforms change it.
struct NoteData
{
    te::MidiClip* clip = nullptr;
    int pitch = 60;
    te::BeatPosition start {}; // in the song
    te::BeatDuration length = te::BeatDuration::fromBeats (1.0);
    int velocity = 100;
    bool muted = false;

    te::BeatPosition getEnd() const { return start + length; }
};

// A note, with the clip it is in.
struct ClipNote
{
    te::MidiClip* clip = nullptr;
    te::MidiNote* note = nullptr;
};

te::BeatPosition getNoteStart (const te::MidiClip&, const te::MidiNote&); // in the song
NoteData getNoteData (te::MidiClip&, const te::MidiNote&);
std::vector<NoteData> getNoteData (const std::vector<ClipNote>&);

// Notes are at least this long.
te::BeatDuration getMinimumNoteLength();

//==============================================================================
// The selected notes: Tracktion's SelectedMidiEvents, while the selection manager holds it.
te::SelectedMidiEvents* getSelectedMidiEvents (const te::SelectionManager&);
std::vector<ClipNote> getSelectedNotes (const te::SelectionManager&);
bool hasSelectedNotes (const te::SelectionManager&);
juce::Array<te::MidiNote*> toMidiNotes (const std::vector<ClipNote>&); // as SelectedMidiEvents takes them

//==============================================================================
// Edits, with the clip's Edit's undo manager: a command's undo step holds them.

// Adds a note at a song beat. If it ends after the clip, the clip grows to the end of that bar, but only into the empty
// space after it: never past the start of the next clip on its track. A note beyond that stays in the clip without
// playing, as outside a trimmed clip. A looped clip never grows.
te::MidiNote* addNote (te::MidiClip&, int pitch, te::BeatPosition start, te::BeatDuration, int velocity);

// Moves a note to a song beat and a pitch. It can't start before its clip's content does; the clip grows to hold it,
// as addNote() says.
void moveNote (te::MidiClip&, te::MidiNote&, te::BeatPosition start, int pitch);

// Sets a note's length (at least getMinimumNoteLength()); the clip grows to hold it, as addNote() says.
void setNoteLength (te::MidiClip&, te::MidiNote&, te::BeatDuration);

void deleteNotes (const std::vector<ClipNote>&);

// Transposes the notes by semitones, all by the same amount and none beyond the MIDI range.
void transposeNotes (const std::vector<ClipNote>&, int semitones);

// Moves the notes in time, all by the same amount and none before its clip's content.
void nudgeNotes (const std::vector<ClipNote>&, te::BeatDuration);

// Makes the notes longer or shorter by the same amount, each at least getMinimumNoteLength().
void changeNoteLengths (const std::vector<ClipNote>&, te::BeatDuration);

bool areAllNotesMuted (const std::vector<ClipNote>&);
void toggleNotesMuted (const std::vector<ClipNote>&); // mutes them, or unmutes them if they all are
void setNoteVelocities (const std::vector<ClipNote>&, int velocity);

// Copies the notes right after them, as FL Studio's Ctrl+B does, each into its original's clip, which grows as
// addNote() says. Returns the copies.
std::vector<ClipNote> duplicateNotes (const std::vector<ClipNote>&);

// Replaces notes with others, such as a transform's result: the old ones go, and the new ones are added to
// the clips they name. Returns the new notes.
std::vector<ClipNote> replaceNotes (const std::vector<ClipNote>& old, const std::vector<NoteData>& replacements);

// Replaces the selected notes with what a transform makes of them (legato, chop), and selects the new notes. Returns
// them; with no notes selected, nothing changes.
std::vector<ClipNote> replaceSelectedNotes (te::SelectionManager&,
                                            const std::function<std::vector<NoteData> (std::vector<NoteData>)>& transform);

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

struct Key
{
    int root = 0; // 0 is C, 1 C sharp, ..., 11 B
    te::Scale::ScaleType scale = te::Scale::major;
};

// The song's key at a beat, from Tracktion's pitch sequence (which holds its key changes).
Key getKeyAt (const te::Edit&, te::BeatPosition);
juce::String describeKey (const Key&); // "C major"
bool isInKey (int pitch, const Key&);

// The scales offered for a key, without Tracktion's duplicates (ionian is major, aeolian minor).
juce::Array<te::Scale::ScaleType> getKeyScales();

// A scale as projects save it ("major", "dorian", "harmonicMinor"...; ionian as "major" and aeolian as "minor", which
// have the same notes), and back: nothing for a name that isn't one of them.
juce::String getSavedScaleName (te::Scale::ScaleType);
std::optional<te::Scale::ScaleType> findScale (const juce::String& savedName);

// A track's key, which the piano roll shades its rows by and can snap notes to. A track has none until one
// is chosen: Phazite keeps it per track, since Tracktion's pitch sequence is one key for the whole song.
std::optional<Key> getTrackKey (const te::Track&);

// Sets a track's key (or none), with the Edit's undo manager. The song remembers it, and new tracks get it
// (applyLastKey()).
void setTrackKey (te::Track&, const std::optional<Key>&);
void setTrackKeys (const juce::Array<te::Track*>&, const std::optional<Key>&); // the same key for each of them
void applyLastKey (te::Track&); // the key set last on any of the song's tracks, if there is one

// The pitch in the key nearest to a pitch (the lower one when two are as near).
int snapToKey (int pitch, const Key&);

// Transposes by semitones, or with a key, by steps of its scale (a note outside the scale first goes to the
// nearest one in it). Pitches stay within the MIDI range.
std::vector<NoteData> transpose (std::vector<NoteData>, int steps, const std::optional<Key>& diatonicIn);

//==============================================================================
// Chords the piano roll stamps with one click.

struct ChordShape
{
    const char* name; // "Major", "m7"
    te::Chord::ChordType type;
};

const std::vector<ChordShape>& getChordShapes();
std::vector<int> getChordPitches (int root, te::Chord::ChordType); // those in the MIDI range

//==============================================================================
// Drum mode: rows named after the drums instead of piano keys (docs/design.md, section 7).

struct DrumRow
{
    int note = 36;
    juce::String name;
};

// General MIDI's drum for a note ("Kick", "Snare", "Closed Hat"...), or the note's name outside that map.
juce::String getDrumName (int note);

// Whether the piano roll shows the track's clips in drum mode: on for a track whose instrument is
// Tracktion's sampler, unless it was turned off, and for any track it was turned on for.
bool isDrumTrack (const te::AudioTrack&);
void setDrumTrack (te::AudioTrack&, bool);

// What a drum track's rows come from (the piano roll lays them out: ui/pianoroll/DrumRows.h). The sampler's sounds,
// one row for each note it plays (its first sound for that note), in the sampler's order: nothing for a track without
// a sampler, and no rows for a sampler without sounds.
std::optional<std::vector<DrumRow>> getSamplerSounds (const te::AudioTrack&);
std::vector<int> getNoteNumbersPlayed (const juce::Array<te::MidiClip*>&); // from low to high, each once

// The track keeps its rows' order and the hidden rows, as view settings without undo steps.
std::vector<int> getDrumRowOrder (const te::AudioTrack&);
void setDrumRowOrder (te::AudioTrack&, const std::vector<int>& notes);
std::vector<int> getHiddenDrumRows (const te::AudioTrack&);
void hideDrumRow (te::AudioTrack&, int note);
void showAllDrumRows (te::AudioTrack&);

//==============================================================================
// The note commands, for the selected notes (Command::notePriority, so the arrow keys move notes while
// notes are selected): transpose by a semitone or an octave, lengthen and shorten by a step of the grid,
// legato, and chop on the grid. getGridStepInBeats gives the piano roll's grid.
void addNoteCommands (CommandRegistry&, te::SelectionManager&, std::function<double()> getGridStepInBeats);
} // namespace phazite
