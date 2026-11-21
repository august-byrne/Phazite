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

// A note's or a controller's MIDI channel in the song (schema::noteChannel, controlChannel), from 1 to 16: 1 where none is
// written, and one beyond them kept within them. And written so, as nothing for the first.
int readChannel (const juce::ValueTree& element, const juce::Identifier& property);
void writeChannel (juce::ValueTree& element, const juce::Identifier& property, int channel, juce::UndoManager*);

// How many notes were read from the song as NoteData since the program started: for the speed gates
// (tests/PerformanceGateTests.cpp), which count what a view's mouse event reads.
std::uint64_t countNotesRead() noexcept;

// The notes of a clip that play: those in the part of the song the clip covers, one that an edge of the clip cuts too
// (which plays from the clip's start, or to its end, as model/playback/internal/SongPlayer.h plays it). A split or a trim
// leaves the others in the clip, as they are, until the clip covers them again: no editor shows them, and no edit of
// what is shown selects or changes them. And whether a note is one that plays, and whether a new note starting at a song
// beat would be: in the clip, or in the room it can grow into (addNote()).
std::vector<NoteData> getClipNotesPlayed (const Song&, ClipID);
bool isNotePlayed (const Song&, const NoteRef&);
bool canPlayNoteAt (const Song&, ClipID, BeatPosition start);

// The latest a note of a clip can start and still play: the shortest note before where the clip can grow to, the start of
// the next clip on its track. None while nothing keeps the clip from growing to hold the note.
std::optional<BeatPosition> getLatestNoteStart (const Song&, ClipID);

//==============================================================================
// The selected notes still there that play (getClipNotesPlayed()), of the clips the piano roll shows (Selection:
// getNotes()); and selecting notes (of those clips), deselecting anything else.
std::vector<NoteRef> getSelectedNotes (const Selection&);
bool hasSelectedNotes (const Selection&);
void selectNotes (Selection&, const std::vector<NoteRef>&);

//==============================================================================
// Edits, with the song's undo history: a command's undo step holds them. Each is made to the note in each of its clip's
// aliases too.

// Adds a note at a song beat. If it ends after the clip, the clip grows to the end of that bar, but only into the empty
// space after it: never past the start of the next clip on its track. A note beyond that stays in the clip without
// playing, as outside a trimmed clip. On a MIDI channel (1 to 16): the first, unless one is given (a note recorded on
// another, or read from a file). Returns it, or none for a clip that isn't a MIDI clip of the song's.
NoteRef addNote (Song&, ClipID, int pitch, BeatPosition start, BeatDuration, int velocity, int channel = 1);

// The kinds of controller events a recording takes in with the notes (schema::CONTROL; NoteValues.h: ControlKind).
const char* getSchemaKind (ControlKind) noexcept;                            // its controlKind in the song
std::optional<ControlKind> findControlKind (const juce::ValueTree& control); // none for a kind Phazite doesn't know

// Adds a controller's value at a song beat, on a MIDI channel (1 to 16), as a recording takes it in with the notes. The
// clip doesn't grow for it: one past the clip's end or before its start plays nothing. Nothing for a clip that isn't a
// MIDI clip of the song's.
void addControl (Song&, ClipID, BeatPosition, ControlKind, int number, int value, int channel = 1);

// The same for many at once, into one clip, each as addControl() says: what a take recorded (a bend sweep holds thousands).
// The clip and each of its aliases is filled as a copy and put in its place (one removal and one insertion, the same clip by
// its ID), so that what listens to the song hears one change, and Undo takes it all back.
struct ControlValue
{
    BeatPosition beat;
    ControlKind kind = ControlKind::controller;
    int number = 0, value = 0;
    int channel = 1; // from 1 to 16
};

void addControls (Song&, ClipID, const std::vector<ControlValue>&);

// Removes a controller's values on one MIDI channel (by its number, if its kind has one) from a clip and its aliases, from
// one song beat to another, both included: what a recording made with Overdub takes the place of. And one value alone, at
// a beat: what a take after a jump that carries on what the take before let go of at its end takes away (Take.h).
void removeControls (Song&, ClipID, ControlKind, int number, int channel, BeatPosition from, BeatPosition to);
void removeControl (Song&, ClipID, const ControlValue&);

// The piano roll's controller lanes (NoteValues.h: ControlLane), with the song's undo history. As notes are, only the
// values that play are shown and changed: those in the part of the song their clip covers (from its start, up to its end),
// as model/playback/internal/SongPlayer.h plays them; a split or a trim leaves the others in the clip as they are. Every
// change is made in each of the clip's aliases too.

// The lanes these clips hold values of that play, in the order the lane menu lists them (NoteValues.h), each once. And a
// lane's values that play, clip by clip, each clip's in time order (two at one beat in the order the clip holds them).
std::vector<ControlLane> getControlLanes (const Song&, const std::vector<ClipID>&);
std::vector<ControlPoint> getControlValues (const Song&, const std::vector<ClipID>&, const ControlLane&);

// Replaces a lane's values in a clip that play from one song beat to another, both included, with these (their clips
// aside: they go into this one). Of them, those outside the range or where the clip doesn't play them are left out, one
// closer than getShortestControlGap() after the one before takes its place, and one that repeats the value before it (the
// clip's, before the range) is left out, as a recording leaves it out. A clip doesn't grow for a value. Nothing for a note's
// pressure, which is not drawn. A value already there as it would be put stays as it is, the others are removed and added:
// values set as they are change nothing. One walk through the clip's elements for each alias, which reads no note: a step
// of a drag.
void setControlValues (Song&, ClipID, const ControlLane&, BeatRange, const std::vector<ControlPoint>&);

// Removes a lane's values that play from these clips from one song beat to another, both included; or all of them. Many
// values removed (more than a step of a drag removes) are one change of each clip: it is filled as a copy without them and
// put in its place, the same clip by its ID, as addControls() fills one; a few are removed in place.
void deleteControlValues (Song&, const std::vector<ClipID>&, const ControlLane&, BeatRange);
void deleteAllControlValues (Song&, const std::vector<ClipID>&, const ControlLane&);

// Moves a note to a song beat and a pitch. It can't go to before its clip's start (one that starts before it already,
// which a split or a trim cut, goes no earlier than it is); the clip grows to hold it, as addNote() says, and where the
// clip can't grow it goes no later than getLatestNoteStart() (nor than it is, if it is later already): a move never
// takes a note that plays to where its clip doesn't play it.
void moveNote (Song&, const NoteRef&, BeatPosition start, int pitch);

// Sets a note's length (at least getMinimumNoteLength()); the clip grows to hold it, as addNote() says.
void setNoteLength (Song&, const NoteRef&, BeatDuration);

// Both at once, as moveNote() and setNoteLength() say: the clip grows only to hold the note as it ends up, not as it is
// between the two (moved with its old length, or made longer from its old start).
void placeNote (Song&, const NoteRef&, BeatPosition start, int pitch, BeatDuration);

// Each note (by its clip and ID; those gone left out) placed as placeNote() says, at its start, pitch and length: a step of
// the piano roll's drag. Where each clip can grow to is found once for all of them, not once for each note.
void placeNotes (Song&, const std::vector<NoteData>&);

void deleteNotes (Song&, const std::vector<NoteRef>&);

// Transposes the notes by semitones, all by the same amount and none beyond the MIDI range.
void transposeNotes (Song&, const std::vector<NoteRef>&, int semitones);

// Moves the notes in time, all by the same amount and none to before its clip's start or to where its clip doesn't play
// it, as moveNote() says.
void nudgeNotes (Song&, const std::vector<NoteRef>&, BeatDuration);

// Makes the notes longer or shorter by the same amount, each at least getMinimumNoteLength().
void changeNoteLengths (Song&, const std::vector<NoteRef>&, BeatDuration);

bool areAllNotesMuted (const Song&, const std::vector<NoteRef>&);
void toggleNotesMuted (Song&, const std::vector<NoteRef>&); // mutes them, or unmutes them if they all are
void setNoteVelocities (Song&, const std::vector<NoteRef>&, int velocity);
void setNoteChannels (Song&, const std::vector<NoteRef>&, int channel); // 1 to 16, kept within them

// Slide and portamento notes (NoteValues.h), switched on for all of them, or off if all are, in the caller's undo step.
bool areAllNotesSlides (const Song&, const std::vector<NoteRef>&);
void toggleNotesSlide (Song&, const std::vector<NoteRef>&);
bool areAllNotesPortamento (const Song&, const std::vector<NoteRef>&);
void toggleNotesPortamento (Song&, const std::vector<NoteRef>&);

// Raises or lowers each note's velocity by the same amount, as one undo step, "Change Velocity" (Alt and the wheel).
void changeNoteVelocities (Song&, const std::vector<NoteRef>&, int change);

// Copies the notes right after them, as FL Studio's Ctrl+B does, each into its original's clip, which grows as
// addNote() says; a copy that wouldn't play, past its clip where the clip can't grow, isn't made, and then the step named
// stepName is named for the copies made (tellNotesMade()). Returns the copies.
std::vector<NoteRef> duplicateNotes (Song&, const std::vector<NoteRef>&, const juce::String& stepName = {});

// Where fewer notes were made than were asked for (copies or pasted notes that would land past a clip which the next clip
// on its track keeps from growing), the undo step in progress, begun as `asked` for all of them, is named for those made
// ("Duplicate 2 Notes" for "Duplicate 6 Notes"), and the hint bar says how many didn't fit (UndoStep::tellOutcome(),
// model/commands/internal/Undo.h). Nothing when every one was made.
void tellNotesMade (Song&, const juce::String& asked, const juce::String& verb, size_t wanted, size_t made);

// Puts these notes in a clip, and in its aliases, in place of all it holds, without growing it: the notes the chord lane
// makes for a part. Each gets a new ID.
void setClipNotes (Song&, ClipID, const std::vector<NoteData>&);

// Replaces notes with others, such as a transform's result: the old ones go, and the new ones are added to the clips
// they name. One that replaces an old one (it has its ID, which it keeps) stays where its clip plays it, as a moved note
// does (moveNote()); the others that come of an old one (a chopped note's later pieces, which have its ID too) go where
// they are given, past the clip's end too, where the old note reached; and one that comes of none (a copy, a pasted note)
// is added only where it plays (getClipNotesPlayed()). Returns the new notes.
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
