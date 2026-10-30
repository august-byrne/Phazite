// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist's tracks: which ones it shows, their colors, and the track commands. The song holds them in Phazite's
// own record of it (schema::TRACKS; docs/tracktion-migration.md, T1): what each is, its name and color, mute and solo,
// and how it shows. Tracktion's tracks mirror them (model/mirror/internal/Mirror.h), with the same IDs, and hold what is
// still Tracktion's: their clips and devices.
#pragma once

#include "model/arrangement/TrackColours.h"
#include "model/base/internal/Selection.h"
#include "model/base/internal/Tracktion.h"

#include <optional>
#include <vector>

namespace phazite
{
class CommandRegistry;

//==============================================================================
// The song's tracks as Phazite's record holds them: a TRACK element for each (Schema.h), folders holding theirs.

// The track list, which every project has (Project adds it as the project opens, with the master's element, MASTER):
// added if the song has none yet, without an undo step; or found without adding it (then invalid).
juce::ValueTree getTrackList (te::Edit&);
juce::ValueTree findTrackList (const te::Edit&);
juce::ValueTree getMasterState (te::Edit&);

// A track's element, anywhere in the list; invalid for one that is gone. And each track's, top to bottom, every folder
// before its tracks.
juce::ValueTree findTrackState (const te::Edit&, TrackID);
juce::ValueTree findTrackState (const te::Track&);
std::vector<juce::ValueTree> getTrackStates (const te::Edit&);

TrackID getTrackID (const juce::ValueTree& trackState);
bool isFolderState (const juce::ValueTree& trackState);
bool isReturnState (const juce::ValueTree& trackState);

// A track's name as the song shows it: its own, or for a track or a return without one, its number among them, "Track 3",
// as Tracktion numbers them.
juce::String getTrackName (const te::Track&);

// What a track is: a return (it takes in what tracks send to it, and no solo silences it).
bool isReturnTrack (const te::Track&);

// The folder a track is in, or nullptr.
te::FolderTrack* getParentFolder (const te::Track&);

// Mute and solo, each track's own switch: muted with its folders' mutes too, or only its own.
bool isTrackMuted (const te::Track&, bool orInMutedFolder);
bool isTrackSoloed (const te::Track&);
bool areAnyTracksSoloed (const te::Edit&);

// The switches set, not as undo steps (as in many DAWs); a mute in the caller's undo step with its undo manager (a bounce
// mutes the track it rendered as part of its step).
void setTrackMuted (te::Track&, bool, juce::UndoManager* = nullptr);
void setTrackSoloed (te::Track&, bool);

// A track's name and color, in the caller's undo step (renameTrack() and setTrackColours() are steps of their own).
void setTrackName (te::Track&, const juce::String&);
void setTrackColour (te::Track&, juce::Colour);

// An empty track after another (at the end if that is nullptr), with no color or key of its own (its row's color in the
// palette), not selected, as Tracktion makes one: the track a bounce renders onto, and those a new song starts with.
te::AudioTrack* insertPlainTrack (te::Edit&, te::Track* after);

//==============================================================================
// A track's color: its own, or else the palette's color for its row, so every track has one.
juce::Colour getTrackColour (const te::Track&, int row);

//==============================================================================
// The tracks the playlist shows, top to bottom: tracks, returns and folders, including the tracks inside folders.
// Tracktion's global tracks (tempo, markers, chords, arranger) and the master aren't shown.
juce::Array<te::Track*> getPlaylistTracks (const te::Edit&);

// The selected tracks, top to bottom.
juce::Array<te::Track*> getSelectedTracks (const Selection&);

// The same, without the tracks in a selected folder: those go wherever their folder goes.
juce::Array<te::Track*> getSelectedOutermostTracks (const Selection&);

// The place right after a track, in the same folder. (Tracktion's TrackInsertPoint (track, false)
// means the start of the list for the first top-level track.)
te::TrackInsertPoint getInsertPointAfter (te::Track&);

//==============================================================================
// View settings the project keeps for each track, without undo steps; their on-screen meaning is the playlist's
// (ui/playlist/TrackHeights.h).
std::optional<int> getSavedTrackHeight (const te::Track&); // schema::trackHeight as saved, nothing if never set
void setSavedTrackHeight (te::Track&, int height);         // as given: the playlist clamps

//==============================================================================
// Inserts an empty track after another one (at the end if that is nullptr), in the palette's next
// color, and selects it alone. Returns nullptr if the Edit already has as many tracks as it can hold.
te::AudioTrack* insertTrack (te::Edit&, te::Track* after, Selection*);

// The same for a return, at the end, named (model/mixer/internal/Mixer.h gives it what it takes in).
te::AudioTrack* insertReturnTrack (te::Edit&, const juce::String& name, Selection*);

// The song's tracks (not its folders or returns), top to bottom, after adding empty ones at the end until there are as many
// as asked for: what a new song starts with.
juce::Array<te::AudioTrack*> ensureTracks (te::Edit&, int count);

// For a new song whose Edit Tracktion has just made with its first track, as it makes one while it makes the Edit: that
// track, recorded as the song's first, without an undo step. While T1 lasts: once Tracktion's Edit is made from the song,
// the song's first track is the song's from the start (docs/tracktion-migration.md).
void recordFirstTrack (te::Edit&);

//==============================================================================
// Folders group tracks, and every new one is a bus, which mixes them through its own devices and fader
// (model/mixer/internal/Buses.h; docs/design.md, section 5).

// Inserts an empty folder after a track (at the end if that is nullptr), and selects it alone.
te::FolderTrack* insertFolder (te::Edit&, te::Track* after, Selection*);

// Puts tracks into a new folder where the first of them was, in the order given, and selects the folder.
te::FolderTrack* groupIntoFolder (const juce::Array<te::Track*>&, Selection*);

// Moves a track into a folder, after its last track.
void moveTrackIntoFolder (te::Track&, te::FolderTrack&);

// Takes a folder's tracks out, to where the folder was, and deletes the folder.
void ungroupFolder (te::FolderTrack&);

// A collapsed track is a thin row in the playlist, with only its name, and a collapsed folder hides its tracks.
// A pinned track stays in view while the others scroll. Projects keep both, without undo steps, as they keep
// track heights.
bool isCollapsed (const te::Track&);
void setCollapsed (te::Track&, bool);
bool isPinned (const te::Track&);
void setPinned (te::Track&, bool);

// True if the track is inside a collapsed folder, so the playlist doesn't show it.
bool isHiddenInCollapsedFolder (const te::Track&);

// Solo, which Tracktion keeps apart from mute: soloing never changes a track's mute switch. Solos a track alone,
// unsoloing the others, as a right-click on FL Studio's mute switch does; or unsolos it if it is the only one.
void toggleSoloAlone (te::Track&);

// True if a track is silent because other tracks are soloed, though it isn't muted itself.
bool isSilencedBySolo (const te::Track&);

//==============================================================================
// What the track commands do. These don't start an undo step: the command does (Command::undoTarget).

bool hasSelectedTracks (const Selection&);
juce::Array<te::FolderTrack*> getSelectedFolders (const Selection&); // top to bottom
bool hasSelectedFolders (const Selection&);

// The selected tracks, or all of the playlist's if none is selected; none without an Edit.
juce::Array<te::Track*> getSelectedTracksOrAll (const Selection&);

// Inserts an empty track or folder after the last selected track, or at the end, and selects it alone. nullptr
// without an Edit.
te::AudioTrack* insertTrackAfterSelection (Selection&);
te::FolderTrack* insertFolderAfterSelection (Selection&);

// Takes the tracks out of each folder, and deletes the folders.
void ungroupFolders (const juce::Array<te::FolderTrack*>&);

// Selects the track above (direction -1) or below (1) the selection; with nothing selected, the last or the first track.
// It stops at the first and the last track.
void selectNeighbourTrack (Selection&, int direction);

// Moves the selected tracks one place up (direction -1) or down (1) within their folders. A track swaps places with its
// neighbour, so selected neighbours move together.
void moveSelectedTracks (Selection&, int direction);

// Switches for lists of tracks (base/Toggles.h): on for all of them, or off if all are on. Mute and solo are each
// track's own switch, not a folder's that it is in. Collapsing and pinning change the view: they aren't undo steps.
bool areAllTracksMuted (const juce::Array<te::Track*>&);
void toggleTracksMuted (const juce::Array<te::Track*>&);
bool areAllTracksSoloed (const juce::Array<te::Track*>&);
void toggleTracksSoloed (const juce::Array<te::Track*>&);
bool areAllTracksCollapsed (const juce::Array<te::Track*>&);
void toggleTracksCollapsed (const juce::Array<te::Track*>&);
bool areAllTracksPinned (const juce::Array<te::Track*>&);
void toggleTracksPinned (const juce::Array<te::Track*>&);

// Copies each track right after its original, and returns the copies. With a selection given, the copies are selected
// instead of what was; without one, what is selected stays as it was.
juce::Array<te::Track*> duplicateTracks (const juce::Array<te::Track*>&, Selection*);

// Renames a track, as one undo step; an empty name gives it its number back. The same name changes nothing.
void renameTrack (te::Track&, const juce::String& name);

// Adds a track, or a folder, after the last one, as one undo step ("Add Track", "Add Folder"), and selects it if a selection
// manager is given. Returns it, or nullptr.
te::Track* addTrackAtEnd (te::Edit&, bool folder, Selection*);

// Gives tracks a color, as one undo step ("Change Track Color", or "Colors" for several).
void setTrackColours (const juce::Array<te::Track*>&, juce::Colour);

// Moves tracks, in their order, to right after another (nullptr: to the start), as one undo step ("Move Track(s)").
void moveTracksAfter (const juce::Array<te::Track*>& moving, te::Track* previous);

// Deletes tracks; a folder goes with the tracks inside it, and devices whose sidechain listened to one of them listen
// to their own sound again. With a selection manager, the track that takes the first one's place is selected, as in
// Ableton Live, so the keyboard can carry on from there.
void deleteTracks (const juce::Array<te::Track*>&, Selection*);

// The track commands that need no window: add, select the track above or below, move, mute and solo. They act
// on the tracks selected in the selection manager's Edit. Delete and duplicate are selection commands
// (Selection.h); the playlist adds rename and color, and the taller and shorter commands (TrackHeights.h).
void addTrackCommands (CommandRegistry&, Selection&);
} // namespace phazite
