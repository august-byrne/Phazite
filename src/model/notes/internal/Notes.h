// SPDX-License-Identifier: AGPL-3.0-or-later
// Notes in MIDI clips, as the piano roll edits them (docs/design.md, section 7): the song's side of NoteValues.h, which
// holds notes as plain values with the transforms, keys, chords and drums' names. The song holds each note in its clip's
// element (schema::NOTE), and in each of the clip's aliases', by its ID. Positions are song beats (the clip's content start
// plus the note's own beat), so the notes of several clips line up.
#pragma once

#include "model/base/Beats.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/base/internal/Selection.h"
#include "model/notes/NoteValues.h"

#include <functional>
#include <optional>
#include <vector>

namespace phazite
{
class CommandRegistry;

// A note's element, invalid for a note that is gone; and what it is, by where it is in the song. Notes' IDs
// (schema::noteID) are each note's, which no other note of its clip has, and the same in its clip's aliases; a copy of a
// clip keeps them.
juce::ValueTree findNoteState (const Song&, const NoteRef&);
std::optional<NoteData> getNoteData (const Song&, const NoteRef&);
std::vector<NoteData> getNoteData (const Song&, const std::vector<NoteRef>&); // those there
std::vector<NoteData> getClipNotes (const Song&, ClipID);                     // in the order the clip holds them

//==============================================================================
// The selected notes still there, of the clips the piano roll shows (Selection: getNotes()); and selecting notes (of those
// clips), deselecting anything else.
std::vector<NoteRef> getSelectedNotes (const Selection&);
bool hasSelectedNotes (const Selection&);
void selectNotes (Selection&, const std::vector<NoteRef>&);

//==============================================================================
// Edits, with the song's undo history: a command's undo step holds them. Each is made to the note in each of its clip's
// aliases too.

// Adds a note at a song beat. If it ends after the clip, the clip grows to the end of that bar, but only into the empty
// space after it: never past the start of the next clip on its track. A note beyond that stays in the clip without
// playing, as outside a trimmed clip. Returns it, or none for a clip that isn't a MIDI clip of the song's.
NoteRef addNote (Song&, ClipID, int pitch, BeatPosition start, BeatDuration, int velocity);

// Moves a note to a song beat and a pitch. It can't start before its clip's content does; the clip grows to hold it,
// as addNote() says.
void moveNote (Song&, const NoteRef&, BeatPosition start, int pitch);

// Sets a note's length (at least getMinimumNoteLength()); the clip grows to hold it, as addNote() says.
void setNoteLength (Song&, const NoteRef&, BeatDuration);

void deleteNotes (Song&, const std::vector<NoteRef>&);

// Transposes the notes by semitones, all by the same amount and none beyond the MIDI range.
void transposeNotes (Song&, const std::vector<NoteRef>&, int semitones);

// Moves the notes in time, all by the same amount and none before its clip's content.
void nudgeNotes (Song&, const std::vector<NoteRef>&, BeatDuration);

// Makes the notes longer or shorter by the same amount, each at least getMinimumNoteLength().
void changeNoteLengths (Song&, const std::vector<NoteRef>&, BeatDuration);

bool areAllNotesMuted (const Song&, const std::vector<NoteRef>&);
void toggleNotesMuted (Song&, const std::vector<NoteRef>&); // mutes them, or unmutes them if they all are
void setNoteVelocities (Song&, const std::vector<NoteRef>&, int velocity);

// Slide and portamento notes (NoteValues.h), switched on for all of them, or off if all are, in the caller's undo step.
bool areAllNotesSlides (const Song&, const std::vector<NoteRef>&);
void toggleNotesSlide (Song&, const std::vector<NoteRef>&);
bool areAllNotesPortamento (const Song&, const std::vector<NoteRef>&);
void toggleNotesPortamento (Song&, const std::vector<NoteRef>&);

// Raises or lowers each note's velocity by the same amount, as one undo step, "Change Velocity" (Alt and the wheel).
void changeNoteVelocities (Song&, const std::vector<NoteRef>&, int change);

// Copies the notes right after them, as FL Studio's Ctrl+B does, each into its original's clip, which grows as
// addNote() says. Returns the copies.
std::vector<NoteRef> duplicateNotes (Song&, const std::vector<NoteRef>&);

// Puts these notes in a clip, and in its aliases, in place of all it holds, without growing it: the notes the chord lane
// makes for a part. Each gets a new ID.
void setClipNotes (Song&, ClipID, const std::vector<NoteData>&);

// Replaces notes with others, such as a transform's result: the old ones go, and the new ones are added to the clips
// they name. Returns the new notes.
std::vector<NoteRef> replaceNotes (Song&, const std::vector<NoteRef>& old, const std::vector<NoteData>& replacements);

// The same, as one undo step with the name given (a transform's, such as "Quantize Notes").
std::vector<NoteRef> replaceNotes (Song&, const std::vector<NoteRef>& old, const std::vector<NoteData>& replacements,
                                   const juce::String& undoName);

// Replaces the selected notes with what a transform makes of them (legato, chop), and selects the new notes. Returns
// them; with no notes selected, nothing changes.
std::vector<NoteRef> replaceSelectedNotes (Selection&, const std::function<std::vector<NoteData> (std::vector<NoteData>)>& transform);

//==============================================================================
// Keys.

// A track's key, which the piano roll shades its rows by and can snap notes to. A track has none until one is chosen:
// Phazite keeps a key for each track, and the chord lane's keys along the song (model/chords/ChordModel.h).
std::optional<Key> getTrackKey (const Song&, TrackID);

// Sets a track's key (or none), in the caller's undo step. The song remembers it, and new tracks get it (applyLastKey()).
void setTrackKey (Song&, TrackID, const std::optional<Key>&);
void setTrackKeys (Song&, const std::vector<TrackID>&, const std::optional<Key>&); // the same key for each of them

// Sets a track's key as the piano roll's key menu does, as one undo step: "Set Key", or "Turn Key Off".
void chooseTrackKey (Song&, TrackID, const std::optional<Key>&);
void applyLastKey (Song&, TrackID); // the key set last on any of the song's tracks, if there is one

// Makes a key the one new tracks get (applyLastKey()), as setting it on a track does, without an undo step: a new
// project's key, before it has tracks.
void setLastKey (Song&, const Key&);

//==============================================================================
// Drum mode (docs/design.md, section 7).

// Whether the piano roll shows the track's clips in drum mode: on for a track whose instrument is Phazite's drum sampler,
// unless it was turned off, and for any track it was turned on for.
bool isDrumTrack (const Song&, TrackID);
void setDrumTrack (Song&, TrackID, bool);

// What a drum track's rows come from (the piano roll lays them out: ui/pianoroll/DrumRows.h): Phazite's drum sampler's
// pads that play a sound, by their notes, each named after what it plays, or nothing for a track without one.
std::optional<std::vector<DrumRow>> getSamplerSounds (const Song&, TrackID);
std::vector<int> getNoteNumbersPlayed (const Song&, const std::vector<ClipID>&); // from low to high, each once

// The track keeps its rows' order and the hidden rows, as view settings without undo steps.
std::vector<int> getDrumRowOrder (const Song&, TrackID);
void setDrumRowOrder (Song&, TrackID, const std::vector<int>& notes);
std::vector<int> getHiddenDrumRows (const Song&, TrackID);
void hideDrumRow (Song&, TrackID, int note);
void showAllDrumRows (Song&, TrackID);

//==============================================================================
// The note commands, for the selected notes (Command::notePriority, so the arrow keys move notes while
// notes are selected): transpose by a semitone or an octave, lengthen and shorten by a step of the grid,
// legato, and chop on the grid. getGridStepInBeats gives the piano roll's grid, and getLegatoGapBeats the gap its
// Legato... was given last (none without it).
void addNoteCommands (CommandRegistry&, Selection&, std::function<double()> getGridStepInBeats,
                      std::function<double()> getLegatoGapBeats = {});
} // namespace phazite
