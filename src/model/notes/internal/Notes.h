// SPDX-License-Identifier: AGPL-3.0-or-later
// Notes in MIDI clips, as the piano roll edits them (docs/design.md, section 7): the Tracktion side of NoteValues.h, which
// holds notes as plain values with the transforms, keys, chords and drums' names. Positions are song beats (the clip's
// content start plus the note's own beat), so the notes of several clips line up.
#pragma once

#include "model/base/internal/Tracktion.h"
#include "model/notes/NoteValues.h"

#include <functional>
#include <optional>
#include <vector>

namespace phazite
{
class CommandRegistry;

// A note, with the clip it is in.
struct ClipNote
{
    te::MidiClip* clip = nullptr;
    te::MidiNote* note = nullptr;
};

te::BeatPosition getNoteStart (const te::MidiClip&, const te::MidiNote&); // in the song
NoteData getNoteData (const te::MidiClip&, const te::MidiNote&);
std::vector<NoteData> getNoteData (const std::vector<ClipNote>&);

// A scale as Tracktion has it, and back (Tracktion's ionian and aeolian are major and minor, which
// have the same notes).
te::Scale::ScaleType toTracktion (Scale);
Scale toScale (te::Scale::ScaleType);

//==============================================================================
// Notes' IDs (schema::phaziteNoteID): each note's, which no other note of its clip has. A copy of a clip keeps them.

NoteID getNoteID (const te::MidiNote&); // none for a note that has no ID yet
te::MidiNote* findNote (const te::MidiClip&, NoteID);
NoteRef getNoteRef (const te::MidiClip&, const te::MidiNote&);

// Gives every note of an Edit that has no ID, or one that another note of its clip has, an ID of its own, without an undo
// step: when a project opens or is saved, and when the project context takes an Edit.
void assignNoteIDs (te::Edit&);

// While it lives, gives every note added to an Edit an ID, without an undo step, from above the highest the Edit had:
// the open project's (ProjectContext). It first assigns those the Edit's notes lack (assignNoteIDs()).
class NoteIDKeeper : private juce::ValueTree::Listener
{
public:
    explicit NoteIDKeeper (te::Edit&);
    ~NoteIDKeeper() override;

private:
    juce::ValueTree state;
    juce::int64 next = 1;

    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;

    JUCE_DECLARE_NON_COPYABLE (NoteIDKeeper)
};

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

// Raises or lowers each note's velocity by the same amount, as one undo step, "Change Velocity" (Alt and the wheel).
void changeNoteVelocities (const std::vector<ClipNote>&, int change);

// Copies the notes right after them, as FL Studio's Ctrl+B does, each into its original's clip, which grows as
// addNote() says. Returns the copies.
std::vector<ClipNote> duplicateNotes (const std::vector<ClipNote>&);

// Replaces notes with others, such as a transform's result: the old ones go, and the new ones are added to the Edit's
// clips they name. Returns the new notes.
std::vector<ClipNote> replaceNotes (te::Edit&, const std::vector<ClipNote>& old, const std::vector<NoteData>& replacements);

// The same, as one undo step with the name given (a transform's, such as "Quantize Notes").
std::vector<ClipNote> replaceNotes (te::Edit&, const std::vector<ClipNote>& old, const std::vector<NoteData>& replacements,
                                    const juce::String& undoName);

// Replaces the selected notes with what a transform makes of them (legato, chop), and selects the new notes. Returns
// them; with no notes selected, nothing changes.
std::vector<ClipNote> replaceSelectedNotes (te::SelectionManager&,
                                            const std::function<std::vector<NoteData> (std::vector<NoteData>)>& transform);

//==============================================================================
// Keys.

// The song's key at a beat, from Tracktion's pitch sequence (which holds its key changes).
Key getKeyAt (const te::Edit&, te::BeatPosition);

// A track's key, which the piano roll shades its rows by and can snap notes to. A track has none until one
// is chosen: Phazite keeps it per track, since Tracktion's pitch sequence is one key for the whole song.
std::optional<Key> getTrackKey (const te::Track&);

// Sets a track's key (or none), with the Edit's undo manager. The song remembers it, and new tracks get it
// (applyLastKey()).
void setTrackKey (te::Track&, const std::optional<Key>&);
void setTrackKeys (const juce::Array<te::Track*>&, const std::optional<Key>&); // the same key for each of them

// Sets a track's key as the piano roll's key menu does, as one undo step: "Set Key", or "Turn Key Off".
void chooseTrackKey (te::Track&, const std::optional<Key>&);
void applyLastKey (te::Track&); // the key set last on any of the song's tracks, if there is one

// Makes a key the one new tracks get (applyLastKey()), as setting it on a track does, without an undo step: a new
// project's key, before it has tracks.
void setLastKey (te::Edit&, const Key&);

//==============================================================================
// Drum mode (docs/design.md, section 7).

// Whether the piano roll shows the track's clips in drum mode: on for a track whose instrument is
// Phazite's drum sampler or Tracktion's sampler, unless it was turned off, and for any track it was turned on for.
bool isDrumTrack (const te::AudioTrack&);
void setDrumTrack (te::AudioTrack&, bool);

// What a drum track's rows come from (the piano roll lays them out: ui/pianoroll/DrumRows.h). Phazite's drum sampler's pads,
// named after what each plays, or Tracktion's sampler's sounds, one row for each note it plays (its first sound for that
// note), in the sampler's order: nothing for a track without a sampler, and no rows for a sampler without sounds.
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
