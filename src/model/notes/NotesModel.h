// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's notes as the piano roll reaches them (docs/design.md, section 7), by IDs and plain values (architecture phase
// A3: no Tracktion here): the MIDI clips it shows and their notes (NoteValues.h), other tracks' notes as ghosts, the
// edits its tools make, a track's key, drum mode and its rows, and notes played as they are placed. Each function takes
// the open project's context, and does nothing, or gives nothing, for an ID of something that is gone. Edits join the
// caller's undo step (a gesture's or a command's), unless their comments say they are steps of their own.
#pragma once

#include "NoteValues.h"

#include <juce_graphics/juce_graphics.h>

#include <functional>
#include <optional>
#include <vector>

namespace phazite
{
class CommandRegistry;
class ProjectContext;

//==============================================================================
// MIDI clips, as the piano roll shows them.
struct NoteClip
{
    ClipID id {};
    TrackID track {};
    juce::String name {};
    BeatRange range {};           // where it is in the song
    BeatPosition contentStart {}; // where its notes count from: bar 1 while the piano roll shows it
    juce::Colour colour {};       // its notes' color: its track's, as the playlist shows it
};

// The MIDI clips among these that are still in the song, in the order given; not automation clips, which hold a curve.
std::vector<NoteClip> getNoteClips (const ProjectContext&, const std::vector<ClipID>&);
std::optional<NoteClip> getNoteClip (const ProjectContext&, ClipID);

// Whether a track can hold MIDI clips (not a folder); and whether it holds audio clips, since audio and notes don't share
// a track (docs/design.md, section 5).
bool canHoldNotes (const ProjectContext&, TrackID);
bool hasAudioClips (const ProjectContext&, TrackID);

// A track's MIDI clip at a beat, or else the one nearest to it; nothing for a track without MIDI clips (automation clips
// don't count).
std::optional<ClipID> getMidiClipNearest (const ProjectContext&, TrackID, BeatPosition);

// A MIDI clip a bar long from a beat, on a track shown without clips, for the first note drawn there; not selected.
// Returns it, or an empty ID.
ClipID addClipForNotes (ProjectContext&, TrackID, BeatPosition start);

// Where an editor showing these clips shows that their notes are playing while a clip linked to them plays
// (Clips.h: getLinkedPlayBeat()).
std::optional<double> getLinkedPlayBeat (const ProjectContext&, const std::vector<ClipID>&, double songBeat);

//==============================================================================
// Notes.

// The notes of these clips, clip by clip, in song beats.
std::vector<NoteData> getNotes (const ProjectContext&, const std::vector<ClipID>&);
std::optional<NoteData> getNote (const ProjectContext&, NoteRef);

// The notes of the MIDI clips on other tracks than these that overlap a range of the song: the piano roll's ghost notes.
std::vector<NoteData> getNotesOfOtherTracks (const ProjectContext&, const std::vector<TrackID>& shown, BeatRange);

// Edits (Notes.h), which join the caller's undo step: a note added at a song beat (Notes.h: addNote() says how its clip
// grows), moved, made longer or shorter, deleted, muted, given a velocity, or moved in time with others.
NoteRef addNote (ProjectContext&, ClipID, int pitch, BeatPosition start, BeatDuration, int velocity);
void moveNote (ProjectContext&, NoteRef, BeatPosition start, int pitch);
void setNoteLength (ProjectContext&, NoteRef, BeatDuration);
void deleteNotes (ProjectContext&, const std::vector<NoteRef>&);
void toggleNotesMuted (ProjectContext&, const std::vector<NoteRef>&);
void setNoteVelocities (ProjectContext&, const std::vector<NoteRef>&, int velocity);
void nudgeNotes (ProjectContext&, const std::vector<NoteRef>&, BeatDuration);

// Gives notes the start, length and pitch of the values given, each named by its clip and ID: a drag's notes as the
// pointer moves. No note starts before its clip, and the clip grows to hold them, as Notes.h's moveNote() says.
void setNotes (ProjectContext&, const std::vector<NoteData>&);

// Replaces notes with others, such as a transform's result, as Notes.h's replaceNotes() does; returns the new notes.
std::vector<NoteRef> replaceNotes (ProjectContext&, const std::vector<NoteRef>& old, const std::vector<NoteData>& replacements);

// Steps of their own: replacing notes, under a name (a transform's, such as "Quantize Notes"); and raising or lowering
// notes' velocity by the same amount, "Change Velocity" (Alt and the wheel).
std::vector<NoteRef> replaceNotes (ProjectContext&, const std::vector<NoteRef>& old, const std::vector<NoteData>& replacements,
                                   const juce::String& undoName);
void changeNoteVelocities (ProjectContext&, const std::vector<NoteRef>&, int change);

//==============================================================================
// A track's key (Notes.h), which the piano roll shades its rows by and can snap notes to.
std::optional<Key> getTrackKey (const ProjectContext&, TrackID);

// Steps of their own: a track's key as the key menu sets it, "Set Key" or "Turn Key Off"; and the same key, or none, on
// every track, "Use This Key on Every Track".
void chooseTrackKey (ProjectContext&, TrackID, const std::optional<Key>&);
void useKeyOnEveryTrack (ProjectContext&, const std::optional<Key>&);

//==============================================================================
// Drum mode (Notes.h): whether the piano roll shows a track's clips as drums, and what its rows come from. View state,
// written without undo steps.
bool isDrumTrack (const ProjectContext&, TrackID);
void setDrumTrack (ProjectContext&, TrackID, bool);
std::optional<std::vector<DrumRow>> getSamplerSounds (const ProjectContext&, TrackID);
std::vector<int> getNoteNumbersPlayed (const ProjectContext&, const std::vector<ClipID>&); // from low to high, each once
std::vector<int> getDrumRowOrder (const ProjectContext&, TrackID);
void setDrumRowOrder (ProjectContext&, TrackID, const std::vector<int>& notes);
std::vector<int> getHiddenDrumRows (const ProjectContext&, TrackID);
void hideDrumRow (ProjectContext&, TrackID, int note);
void showAllDrumRows (ProjectContext&, TrackID);

//==============================================================================
// Notes played on a clip's track and MIDI channel as they are placed, or as their keys are clicked, until they are
// stopped: nothing plays on a track without an instrument. Returns the track they play on, for stopAuditioning(), or an
// empty ID.
TrackID auditionNotes (ProjectContext&, ClipID, const std::vector<int>& pitches, int velocity);
void stopAuditioning (ProjectContext&, TrackID);

//==============================================================================
// The note commands (Notes.h), for the notes selected in the open project: getGridStepInBeats gives the piano roll's
// grid.
void addNoteCommands (CommandRegistry&, ProjectContext&, std::function<double()> getGridStepInBeats);
} // namespace phazite
