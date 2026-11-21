// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's notes as the piano roll reaches them (docs/design.md, section 7), by IDs and plain values: the MIDI clips
// it shows and their notes (NoteValues.h), other tracks' notes as ghosts, the edits its tools make, a track's key, drum
// mode and its rows, and notes played as they are placed. Each function takes the open project's context, and does
// nothing, or gives nothing, for an ID of something that is gone. Edits join the caller's undo step (a gesture's or a
// command's), unless their comments say they are steps of their own.
#pragma once

#include "NoteValues.h"

#include <juce_graphics/juce_graphics.h>

#include <functional>
#include <memory>
#include <optional>
#include <utility>
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

    // The latest a note of it can start and still play, where the next clip on its track keeps it from growing to hold
    // the note; none while it can grow. The edits keep a note that plays no later (Notes.h: moveNote()), and a drag does too.
    std::optional<BeatPosition> latestNoteStart {};
};

// The MIDI clips among these that are still in the song, in the order given.
std::vector<NoteClip> getNoteClips (const ProjectContext&, const std::vector<ClipID>&);
std::optional<NoteClip> getNoteClip (const ProjectContext&, ClipID);

// Whether a track can hold MIDI clips (not a folder or a return); and whether it holds audio clips, since audio and notes
// don't share a track (docs/design.md, section 5).
bool canHoldNotes (const ProjectContext&, TrackID);
bool hasAudioClips (const ProjectContext&, TrackID);

// A track's MIDI clip at a beat, or else the one nearest to it; nothing for a track without MIDI clips.
std::optional<ClipID> getMidiClipNearest (const ProjectContext&, TrackID, BeatPosition);

// A MIDI clip a bar long from a beat, on a track shown without clips, for the first note drawn there; not selected.
// Returns it, or an empty ID.
ClipID addClipForNotes (ProjectContext&, TrackID, BeatPosition start);

// Where an editor showing these clips shows that their notes are playing while a clip linked to them plays
// (Clips.h: getLinkedPlayBeat()).
std::optional<double> getLinkedPlayBeat (const ProjectContext&, const std::vector<ClipID>&, double songBeat);

//==============================================================================
// Notes.

// The notes of these clips, clip by clip, in song beats: all each clip holds; and those of them that play, which are what
// an editor shows, selects and edits: the notes in the part of the song their clip covers, one cut by an edge of its clip
// too. A split or a trim leaves the others in the clip, as they are, until the clip covers them again.
std::vector<NoteData> getNotes (const ProjectContext&, const std::vector<ClipID>&);
std::vector<NoteData> getNotesPlayed (const ProjectContext&, const std::vector<ClipID>&);
std::optional<NoteData> getNote (const ProjectContext&, NoteRef);

// The notes that play of the MIDI clips on other tracks than these that overlap a range of the song: the piano roll's ghost
// notes.
std::vector<NoteData> getNotesOfOtherTracks (const ProjectContext&, const std::vector<TrackID>& shown, BeatRange);

// Edits (Notes.h), which join the caller's undo step: a note added at a song beat (Notes.h: addNote() says how its clip
// grows; none is added where it wouldn't play, after its clip where the clip can't grow to hold it), moved, made longer or
// shorter, deleted, muted, given a velocity, or moved in time with others.
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

// The ends of clips as a drag of their notes began, which each step of the drag puts back first (a clip that grew since
// ends there again; one moved or shortened meanwhile is left as it is), so that a clip grows only as far as the notes are
// now: a note dragged past its clip's end and back leaves the clip as it was. Joins the caller's undo step.
using ClipEnds = std::vector<std::pair<ClipID, BeatPosition>>;
ClipEnds getClipEnds (const ProjectContext&, const std::vector<ClipID>&);
void restoreClipEnds (ProjectContext&, const ClipEnds&);

// Replaces notes with others, such as a transform's result, as Notes.h's replaceNotes() does; returns the new notes.
std::vector<NoteRef> replaceNotes (ProjectContext&, const std::vector<NoteRef>& old, const std::vector<NoteData>& replacements);

// Steps of their own: replacing notes, under a name (a transform's, such as "Quantize Notes"); and raising or lowering
// notes' velocity by the same amount, "Change Velocity" (Alt and the wheel).
std::vector<NoteRef> replaceNotes (ProjectContext&, const std::vector<NoteRef>& old, const std::vector<NoteData>& replacements,
                                   const juce::String& undoName);
void changeNoteVelocities (ProjectContext&, const std::vector<NoteRef>&, int change);

// The wheel raising or lowering notes' velocity over the piano roll's velocity lane: a burst of its moves (a trackpad sends
// many a second) is one step, "Change Velocity", from its first change until end() (the wheel rested, the pointer left the
// lane), or until a step, a gesture, an undo or a redo begins meanwhile, which keeps it first, so that what comes next is a
// step of its own (Undo.h: Trial). Each move of a burst was a step of its own (known bug 55 until 2026-11-14). Message
// thread only.
class VelocityWheel
{
public:
    explicit VelocityWheel (ProjectContext&);
    ~VelocityWheel(); // keeps what the burst changed

    void change (const std::vector<NoteRef>&, int change); // as changeNoteVelocities() does
    void end();
    bool isInBurst() const; // a change made since the burst's start, not kept yet

    struct Impl;

private:
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE (VelocityWheel)
};

// The inspector's edits of notes (docs/design.md, section 4), as Logic Pro's and Bitwig's inspectors edit several at once.
// Each is a step of its own ("Move Notes", "Change Note Length", "Transpose Notes", "Change Velocity"), unless a gesture
// is open (a field's drag: SongGesture.h), whose one step it joins. Changed by an amount (dragged, scrolled or stepped),
// every note changes by it: notes moved (no earlier than each one's clip lets it) or transposed together by as much as
// all of them can go, so that a chord keeps its shape, and each length (at least getMinimumNoteLength(), as a drag keeps
// it), velocity (1 to 127) and MIDI channel (1 to 16) kept in range. Set to a value (typed), each length, velocity and
// channel becomes it, and the earliest start and the lowest pitch move to it, the others with them.
enum class NoteProperty
{
    start,
    length,
    pitch,
    velocity,
    channel
};

void changeNotes (ProjectContext&, const std::vector<NoteRef>&, NoteProperty, double amount);
void setNotesTo (ProjectContext&, const std::vector<NoteRef>&, NoteProperty, double value);

//==============================================================================
// The piano roll's controller lane (NoteValues.h: ControlLane; docs/design/piano-roll.md, "Controller lanes"): what the
// clips hold beside their notes, as Notes.h's functions of the same names say. Only the values that play are shown and
// changed, as notes are.

// The lanes these clips hold values of, in the lane menu's order; and a lane's values, clip by clip, in time order.
std::vector<ControlLane> getControlLanes (const ProjectContext&, const std::vector<ClipID>&);
std::vector<ControlPoint> getControlValues (const ProjectContext&, const std::vector<ClipID>&, const ControlLane&);

// Edits, which join the caller's undo step (a drag's): a lane's values in a clip, and in its aliases, replaced from one
// song beat to another, both included, by these (a drawn line, a ramp: Notes.h says which are left out); and a lane's
// values removed from these clips from one beat to another.
void setControlValues (ProjectContext&, ClipID, const ControlLane&, BeatRange, const std::vector<ControlPoint>&);
void deleteControlValues (ProjectContext&, const std::vector<ClipID>&, const ControlLane&, BeatRange);

// A step of its own, named for the lane ("Delete Sustain Pedal"): every value of a lane that these clips play removed.
void deleteAllControlValues (ProjectContext&, const std::vector<ClipID>&, const ControlLane&);

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
// Notes played on a clip's track as they are placed, or as their keys are clicked, until they are stopped, on a MIDI
// channel (1 to 16): a note's own as it is dragged or clicked, the first for a key clicked or a new note. Nothing plays on a
// track without an instrument. Returns the track they play on, for stopAuditioning(), or an empty ID.
TrackID auditionNotes (ProjectContext&, ClipID, const std::vector<int>& pitches, int velocity, int channel = 1);
void stopAuditioning (ProjectContext&, TrackID);

//==============================================================================
// The note commands (Notes.h), for the notes selected in the open project: getGridStepInBeats gives the piano roll's
// grid, and getLegatoGapBeats the gap its Legato... was given last, which Legato leaves too.
void addNoteCommands (CommandRegistry&, ProjectContext&, std::function<double()> getGridStepInBeats,
                      std::function<double()> getLegatoGapBeats = {});
} // namespace phazite
