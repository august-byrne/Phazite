// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist's tracks: which ones it shows, their colors, and the track commands. The song holds them (schema::TRACKS):
// what each is, its name and color, mute and solo, and how it shows, by their IDs (docs/tracktion-migration.md, T6 step
// 3). What plays them is made from the song.
#pragma once

#include "model/arrangement/PlaylistValues.h"
#include "model/arrangement/TrackColours.h"
#include "model/base/Beats.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/base/internal/Selection.h"

#include <optional>
#include <vector>

namespace phazite
{
class CommandRegistry;

//==============================================================================
// The song's tracks as it holds them: a TRACK element for each (Schema.h), folders holding theirs, and the master's own
// element, MASTER.

// The track list and the master's element, which every song has (a new song starts with them: model/project/internal/
// Project.h, makeNewSong()): added if the song has none yet, without an undo step; or found without adding it (then
// invalid). And the master's ID, which its channel's functions take (MixerModel.h: an empty ID names the master there).
juce::ValueTree getTrackList (Song&);
juce::ValueTree findTrackList (const Song&);
juce::ValueTree getMasterState (Song&);
TrackID getMasterID (const Song&);

// A track's element, anywhere in the list, or the master's; invalid for one that is gone. And each track's, top to bottom,
// every folder before its tracks (not the master), and their IDs: the tracks the playlist shows, tracks, returns and
// folders, and the tracks inside folders.
juce::ValueTree findTrackState (const Song&, TrackID);
std::vector<juce::ValueTree> getTrackStates (const Song&);
std::vector<TrackID> getTrackIDs (const Song&);

TrackID getTrackID (const juce::ValueTree& trackState);
bool isFolderState (const juce::ValueTree& trackState);
bool isReturnState (const juce::ValueTree& trackState);

// What a track is: a folder, which holds tracks; a return (it takes in what tracks send to it, and no solo silences it);
// or the master.
bool isFolder (const Song&, TrackID);
bool isReturnTrack (const Song&, TrackID);
bool isMaster (const Song&, TrackID);

// A track's name as the song shows it: its own, or for a track or a return without one, its number among them, "Track 3".
juce::String getTrackName (const Song&, TrackID);

// The folder a track is in, or none; and the tracks a folder holds, directly or in its folders, top to bottom.
TrackID getParentFolder (const Song&, TrackID);
std::vector<TrackID> getTracksInFolder (const Song&, TrackID folder);

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

// An empty track after another (at the end if that is none), with no color or key of its own (its row's color in the
// palette), not selected: the track a bounce renders onto, and those a new song starts with.
TrackID insertPlainTrack (Song&, TrackID after);

//==============================================================================
// The selected tracks, top to bottom; and the same without the tracks in a selected folder (those go wherever their
// folder goes).
std::vector<TrackID> getSelectedTracks (const Selection&);
std::vector<TrackID> getSelectedOutermostTracks (const Selection&);

//==============================================================================
// Inserts an empty track after another one (at the end if that is none), in the palette's next color, and selects it
// alone if a selection is given. Returns none if it can't.
TrackID insertTrack (Song&, TrackID after, Selection*);

// The same for a return, at the end, named (model/mixer/internal/Mixer.h gives it what it takes in).
TrackID insertReturnTrack (Song&, const juce::String& name, Selection*);

// The song's tracks (not its folders or returns), top to bottom, after adding empty ones at the end until there are as many
// as asked for: what a new song starts with.
std::vector<TrackID> ensureTracks (Song&, int count);

//==============================================================================
// Folders group tracks, and every new one is a bus, which mixes them through its own devices and fader
// (model/mixer/internal/Buses.h; docs/design.md, section 5).

// Inserts an empty folder after a track (at the end if that is none), and selects it alone.
TrackID insertFolder (Song&, TrackID after, Selection*);

// Puts tracks into a new folder where the first of them was, in the order given, and selects the folder.
TrackID groupIntoFolder (Song&, const std::vector<TrackID>&, Selection*);

// Moves a track into a folder, after its last track.
void moveTrackIntoFolder (Song&, TrackID track, TrackID folder);

// Takes a folder's tracks out, to where the folder was, and deletes the folder.
void ungroupFolder (Song&, TrackID folder);

//==============================================================================
// What the track commands do. These don't start an undo step: the command does (Command::undoTarget).

bool hasSelectedTracks (const Selection&);
std::vector<TrackID> getSelectedFolders (const Selection&); // top to bottom
bool hasSelectedFolders (const Selection&);

// The selected tracks, or all of the playlist's if none is selected; none without a song.
std::vector<TrackID> getSelectedTracksOrAll (const Selection&);

// Inserts an empty track or folder after the last selected track, or at the end, and selects it alone. None without a
// song.
TrackID insertTrackAfterSelection (Selection&);
TrackID insertFolderAfterSelection (Selection&);

// Takes the tracks out of each folder, and deletes the folders.
void ungroupFolders (Song&, const std::vector<TrackID>& folders);

// Selects the track above (direction -1) or below (1) the selection; with nothing selected, the last or the first track.
// It stops at the first and the last track.
void selectNeighbourTrack (Selection&, int direction);

// Moves the selected tracks one place up (direction -1) or down (1) within their folders. A track swaps places with its
// neighbour, so selected neighbours move together.
void moveSelectedTracks (Selection&, int direction);

// Switches for lists of tracks (base/Toggles.h): on for all of them, or off if all are on. Mute and solo are each
// track's own switch, not a folder's that it is in. Collapsing and pinning change the view: they aren't undo steps.
bool areAllTracksMuted (const Song&, const std::vector<TrackID>&);
void toggleTracksMuted (Song&, const std::vector<TrackID>&);
bool areAllTracksSoloed (const Song&, const std::vector<TrackID>&);
void toggleTracksSoloed (Song&, const std::vector<TrackID>&);
bool areAllTracksCollapsed (const Song&, const std::vector<TrackID>&);
void toggleTracksCollapsed (Song&, const std::vector<TrackID>&);
bool areAllTracksPinned (const Song&, const std::vector<TrackID>&);
void toggleTracksPinned (Song&, const std::vector<TrackID>&);

// Every row of the playlist collapsed, or every one expanded, as Collapse All does (view state, no undo step): the tracks
// and folders (a collapsed folder hides its tracks), the master's and the returns' rows, and every automation lane; and
// whether any of them is expanded, which tells Collapse All which it does.
bool isAnyRowExpanded (const Song&);
void setAllRowsCollapsed (Song&, bool collapsed);
void toggleAllRowsCollapsed (Song&); // collapses them all while any is expanded, or else expands them all

// Copies each track right after its original, with its devices as they are now, its clips and its automation, and returns
// the copies. With a selection given, the copies are selected instead of what was; without one, what is selected stays as
// it was.
std::vector<TrackID> duplicateTracks (Song&, const std::vector<TrackID>&, Selection*);

// Renames a track, as one undo step; an empty name gives it its number back. The same name changes nothing.
void renameTrack (Song&, TrackID, const juce::String& name);

// Adds a track, or a folder, after the last one, as one undo step ("Add Track", "Add Folder"), and selects it if a selection
// is given. Returns it, or none.
TrackID addTrackAtEnd (Song&, bool folder, Selection*);

// Gives tracks a color, as one undo step ("Change Track Color", or "Colors" for several).
void setTrackColours (Song&, const std::vector<TrackID>&, juce::Colour);

// Moves tracks, in their order, to right after another (none: to the start), as one undo step ("Move Track(s)").
void moveTracksAfter (Song&, const std::vector<TrackID>& moving, TrackID previous);

// Moves tracks where a drag in the playlist drops them (TrackDrop, PlaylistValues.h), in their order in the playlist, a
// folder with the tracks inside it, as one undo step ("Move Track(s)"). Nothing moves where a folder would go into itself
// or into a folder inside it; canMoveTracksTo() says whether they can go there.
bool canMoveTracksTo (const Song&, const std::vector<TrackID>& moving, TrackID target, TrackDrop);
void moveTracksTo (Song&, const std::vector<TrackID>& moving, TrackID target, TrackDrop);

// Deletes tracks; a folder goes with the tracks inside it, and devices whose sidechain listened to one of them listen
// to their own sound again. With a selection, the track that takes the first one's place is selected, as in Ableton
// Live, so the keyboard can carry on from there.
void deleteTracks (Song&, const std::vector<TrackID>&, Selection*);

// The track commands that need no window: add, select the track above or below, move, mute and solo. They act
// on the tracks selected in the selection's song. Delete and duplicate are selection commands (Selection.h); the
// playlist adds rename and color, and the taller and shorter commands (TrackHeights.h).
void addTrackCommands (CommandRegistry&, Selection&);
} // namespace phazite
