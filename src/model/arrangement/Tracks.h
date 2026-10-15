// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist's tracks: which ones it shows, their colors, and the track commands.
#pragma once

#include "TrackColours.h"
#include "model/base/Tracktion.h"

#include <optional>

namespace phazite
{
class CommandRegistry;

//==============================================================================
// A track's color: its own, or else the palette's color for its row, so every track has one.
juce::Colour getTrackColour (const te::Track&, int row);

//==============================================================================
// The tracks the playlist shows, top to bottom: audio tracks and folders, including the tracks inside
// folders. Tracktion's global tracks (tempo, markers, chords, arranger) and the master aren't shown.
juce::Array<te::Track*> getPlaylistTracks (const te::Edit&);

// The selected tracks, top to bottom.
juce::Array<te::Track*> getSelectedTracks (const te::SelectionManager&);

// The same, without the tracks in a selected folder: those go wherever their folder goes.
juce::Array<te::Track*> getSelectedOutermostTracks (const te::SelectionManager&);

// The place right after a track, in the same folder. (Tracktion's TrackInsertPoint (track, false)
// means the start of the list for the first top-level track.)
te::TrackInsertPoint getInsertPointAfter (te::Track&);

//==============================================================================
// View settings the project keeps for each track, without undo steps; their on-screen meaning is the playlist's
// (ui/playlist/TrackHeights.h).
std::optional<int> getSavedTrackHeight (const te::Track&); // schema::phaziteHeight as saved, nothing if never set
void setSavedTrackHeight (te::Track&, int height);         // as given: the playlist clamps

//==============================================================================
// Inserts an empty track after another one (at the end if that is nullptr), in the palette's next
// color, and selects it alone. Returns nullptr if the Edit already has as many tracks as it can hold.
te::AudioTrack* insertTrack (te::Edit&, te::Track* after, te::SelectionManager*);

//==============================================================================
// Folders group tracks. A folder starts out plain (its volume control scales its tracks', as a VCA does)
// and becomes a bus when it holds an effect (docs/design.md, section 5).

// Inserts an empty folder after a track (at the end if that is nullptr), and selects it alone.
te::FolderTrack* insertFolder (te::Edit&, te::Track* after, te::SelectionManager*);

// Puts tracks into a new folder where the first of them was, in the order given, and selects the folder.
te::FolderTrack* groupIntoFolder (const juce::Array<te::Track*>&, te::SelectionManager*);

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

bool hasSelectedTracks (const te::SelectionManager&);
juce::Array<te::FolderTrack*> getSelectedFolders (const te::SelectionManager&); // top to bottom
bool hasSelectedFolders (const te::SelectionManager&);

// The selected tracks, or all of the playlist's if none is selected; none without an Edit.
juce::Array<te::Track*> getSelectedTracksOrAll (const te::SelectionManager&);

// Inserts an empty track or folder after the last selected track, or at the end, and selects it alone. nullptr
// without an Edit.
te::AudioTrack* insertTrackAfterSelection (te::SelectionManager&);
te::FolderTrack* insertFolderAfterSelection (te::SelectionManager&);

// Takes the tracks out of each folder, and deletes the folders.
void ungroupFolders (const juce::Array<te::FolderTrack*>&);

// Selects the track above (direction -1) or below (1) the selection; with nothing selected, the last or the first track.
// It stops at the first and the last track.
void selectNeighbourTrack (te::SelectionManager&, int direction);

// Moves the selected tracks one place up (direction -1) or down (1) within their folders. A track swaps places with its
// neighbour, so selected neighbours move together.
void moveSelectedTracks (te::SelectionManager&, int direction);

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

// Copies each track right after its original, and returns the copies. Tracktion adds each copy to what every selection
// manager of the Edit has selected; with a selection manager given, the copies are selected instead of what was.
juce::Array<te::Track*> duplicateTracks (const juce::Array<te::Track*>&, te::SelectionManager*);

// Renames a track, as one undo step; an empty name gives it its number back. The same name changes nothing.
void renameTrack (te::Track&, const juce::String& name);

// Deletes tracks; a folder goes with the tracks inside it, and devices whose sidechain listened to one of them listen
// to their own sound again. With a selection manager, the track that takes the first one's place is selected, as in
// Ableton Live, so the keyboard can carry on from there.
void deleteTracks (const juce::Array<te::Track*>&, te::SelectionManager*);

// The track commands that need no window: add, select the track above or below, move, mute and solo. They act
// on the tracks selected in the selection manager's Edit. Delete and duplicate are selection commands
// (Selection.h); the playlist adds rename and color, and the taller and shorter commands (TrackHeights.h).
void addTrackCommands (CommandRegistry&, te::SelectionManager&);
} // namespace phazite
