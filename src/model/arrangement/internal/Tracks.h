// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist's tracks: which ones it shows, their colors, and the track commands. The song holds them in Phazite's
// own record of it (schema::TRACKS; docs/tracktion-migration.md, T1): what each is, its name and color, mute and solo,
// and how it shows. Tracktion's tracks mirror them (model/mirror/internal/Mirror.h), with the same IDs, and hold what is
// still Tracktion's: their clips and devices.
#pragma once

#include "model/arrangement/PlaylistValues.h"
#include "model/arrangement/TrackColours.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/Selection.h"
#include "model/base/internal/Tracktion.h"

#include <optional>
#include <vector>

namespace phazite
{
class CommandRegistry;

//==============================================================================
// The song's tracks as Phazite's record holds them: a TRACK element for each (Schema.h), folders holding theirs, and the
// master's own element. Found by their IDs in the song (docs/tracktion-migration.md, T6 step 3); the same for one of
// Tracktion's tracks, until the model's callers hold IDs alone.

// The track list, which every project has (Project adds it as the project opens, with the master's element, MASTER):
// added if the song has none yet, without an undo step; or found without adding it (then invalid).
juce::ValueTree getTrackList (te::Edit&);
juce::ValueTree findTrackList (const Song&);
juce::ValueTree findTrackList (const te::Edit&);
juce::ValueTree getMasterState (te::Edit&);

// A track's element, anywhere in the list, or the master's; invalid for one that is gone. And each track's, top to bottom,
// every folder before its tracks (not the master), and their IDs.
juce::ValueTree findTrackState (const Song&, TrackID);
std::vector<juce::ValueTree> getTrackStates (const Song&);
std::vector<TrackID> getTrackIDs (const Song&);

TrackID getTrackID (const juce::ValueTree& trackState);
bool isFolderState (const juce::ValueTree& trackState);
bool isReturnState (const juce::ValueTree& trackState);

// A track's name as the song shows it: its own, or for a track or a return without one, its number among them, "Track 3".
juce::String getTrackName (const Song&, TrackID);

// What a track is: a return (it takes in what tracks send to it, and no solo silences it).
bool isReturnTrack (const Song&, TrackID);

// The folder a track is in, or none.
TrackID getParentFolder (const Song&, TrackID);

// Mute and solo, each track's own switch: muted with its folders' mutes too, or only its own.
bool isTrackMuted (const Song&, TrackID, bool orInMutedFolder);
bool isTrackSoloed (const Song&, TrackID);
bool areAnyTracksSoloed (const Song&);

// The switches set, not as undo steps (as in many DAWs); a mute in the caller's undo step with its undo manager (a bounce
// mutes the track it rendered as part of its step).
void setTrackMuted (Song&, TrackID, bool, juce::UndoManager* = nullptr);
void setTrackSoloed (Song&, TrackID, bool);

// A track's name and color, in the caller's undo step (renameTrack() and setTrackColours() are steps of their own).
void setTrackName (Song&, TrackID, const juce::String&);
void setTrackColour (Song&, TrackID, juce::Colour);

// A track's color: its own, or else the palette's color for its row, so every track has one.
juce::Colour getTrackColour (const Song&, TrackID, int row);

// View settings the project keeps for each track, without undo steps; their on-screen meaning is the playlist's
// (ui/playlist/TrackHeights.h).
std::optional<int> getSavedTrackHeight (const Song&, TrackID); // schema::trackHeight as saved, nothing if never set
void setSavedTrackHeight (Song&, TrackID, int height);         // as given: the playlist clamps

// A collapsed track is a thin row in the playlist, with only its name, and a collapsed folder hides its tracks.
// A pinned track stays in view while the others scroll. Projects keep both, without undo steps, as they keep
// track heights.
bool isCollapsed (const Song&, TrackID);
void setCollapsed (Song&, TrackID, bool);
bool isPinned (const Song&, TrackID);
void setPinned (Song&, TrackID, bool);

// True if the track is inside a collapsed folder, so the playlist doesn't show it.
bool isHiddenInCollapsedFolder (const Song&, TrackID);

// Solo never changes a track's mute switch. Solos a track alone, unsoloing the others, as a right-click on FL Studio's mute
// switch does; or unsolos it if it is the only one.
void toggleSoloAlone (Song&, TrackID);

// True if a track is silent because other tracks are soloed, though it isn't muted itself.
bool isSilencedBySolo (const Song&, TrackID);

//==============================================================================
// The same for Tracktion's tracks, while the model's callers hold them; and Tracktion's track for a track's element, or
// nullptr.
te::Track* findTrack (const te::Edit&, const juce::ValueTree& trackState);

juce::String getTrackName (const te::Track&); // or Tracktion's name for a track the song hasn't
te::FolderTrack* getParentFolder (const te::Track&);

// clang-format off
inline juce::ValueTree findTrackState (const te::Edit& edit, TrackID id) { return findTrackState (getSong (edit), id); }
inline juce::ValueTree findTrackState (const te::Track& track) { return findTrackState (getSong (track.edit), getTrackID (track)); }
inline std::vector<juce::ValueTree> getTrackStates (const te::Edit& edit) { return getTrackStates (getSong (edit)); }
inline bool isReturnTrack (const te::Track& track) { return isReturnTrack (getSong (track.edit), getTrackID (track)); }
inline bool isTrackMuted (const te::Track& track, bool orInMutedFolder) { return isTrackMuted (getSong (track.edit), getTrackID (track), orInMutedFolder); }
inline bool isTrackSoloed (const te::Track& track) { return isTrackSoloed (getSong (track.edit), getTrackID (track)); }
inline bool areAnyTracksSoloed (const te::Edit& edit) { return areAnyTracksSoloed (getSong (edit)); }
inline void setTrackMuted (te::Track& track, bool muted, juce::UndoManager* undo = nullptr) { setTrackMuted (getSong (track.edit), getTrackID (track), muted, undo); }
inline void setTrackSoloed (te::Track& track, bool soloed) { setTrackSoloed (getSong (track.edit), getTrackID (track), soloed); }
inline void setTrackName (te::Track& track, const juce::String& name) { setTrackName (getSong (track.edit), getTrackID (track), name); }
inline void setTrackColour (te::Track& track, juce::Colour colour) { setTrackColour (getSong (track.edit), getTrackID (track), colour); }
inline juce::Colour getTrackColour (const te::Track& track, int row) { return getTrackColour (getSong (track.edit), getTrackID (track), row); }
inline std::optional<int> getSavedTrackHeight (const te::Track& track) { return getSavedTrackHeight (getSong (track.edit), getTrackID (track)); }
inline void setSavedTrackHeight (te::Track& track, int height) { setSavedTrackHeight (getSong (track.edit), getTrackID (track), height); }
inline bool isCollapsed (const te::Track& track) { return isCollapsed (getSong (track.edit), getTrackID (track)); }
inline void setCollapsed (te::Track& track, bool collapsed) { setCollapsed (getSong (track.edit), getTrackID (track), collapsed); }
inline bool isPinned (const te::Track& track) { return isPinned (getSong (track.edit), getTrackID (track)); }
inline void setPinned (te::Track& track, bool pinned) { setPinned (getSong (track.edit), getTrackID (track), pinned); }
inline bool isHiddenInCollapsedFolder (const te::Track& track) { return isHiddenInCollapsedFolder (getSong (track.edit), getTrackID (track)); }
inline void toggleSoloAlone (te::Track& track) { toggleSoloAlone (getSong (track.edit), getTrackID (track)); }
inline bool isSilencedBySolo (const te::Track& track) { return isSilencedBySolo (getSong (track.edit), getTrackID (track)); }
// clang-format on

// An empty track after another (at the end if that is nullptr), with no color or key of its own (its row's color in the
// palette), not selected, as Tracktion makes one: the track a bounce renders onto, and those a new song starts with.
te::AudioTrack* insertPlainTrack (te::Edit&, te::Track* after);

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

// Every row of the playlist collapsed, or every one expanded, as Collapse All does (view state, no undo step): the tracks
// and folders (a collapsed folder hides its tracks), the master's and the returns' rows, and every automation lane; and
// whether any of them is expanded, which tells Collapse All which it does.
bool isAnyRowExpanded (te::Edit&);
void setAllRowsCollapsed (te::Edit&, bool collapsed);
void toggleAllRowsCollapsed (te::Edit&); // collapses them all while any is expanded, or else expands them all

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

// Moves tracks where a drag in the playlist drops them (TrackDrop, PlaylistValues.h), in their order in the playlist, a
// folder with the tracks inside it, as one undo step ("Move Track(s)"). Nothing moves where a folder would go into itself
// or into a folder inside it; canMoveTracksTo() says whether they can go there.
bool canMoveTracksTo (const juce::Array<te::Track*>& moving, const te::Track& target, TrackDrop);
void moveTracksTo (const juce::Array<te::Track*>& moving, const te::Track& target, TrackDrop);

// Deletes tracks; a folder goes with the tracks inside it, and devices whose sidechain listened to one of them listen
// to their own sound again. With a selection manager, the track that takes the first one's place is selected, as in
// Ableton Live, so the keyboard can carry on from there.
void deleteTracks (const juce::Array<te::Track*>&, Selection*);

// The track commands that need no window: add, select the track above or below, move, mute and solo. They act
// on the tracks selected in the selection manager's Edit. Delete and duplicate are selection commands
// (Selection.h); the playlist adds rename and color, and the taller and shorter commands (TrackHeights.h).
void addTrackCommands (CommandRegistry&, Selection&);
} // namespace phazite
