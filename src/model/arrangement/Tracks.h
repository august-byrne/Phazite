// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist's tracks: which ones it shows, their colors and heights, and the track commands.
#pragma once

#include "model/commands/CommandRegistry.h"

#include <array>

namespace phazite
{
//==============================================================================
// The colors tracks get, in turn: Okabe and Ito's palette (2008), which color-blind users can tell
// apart, with grey instead of black so it shows on dark backgrounds.
struct TrackColour
{
    juce::Colour colour;
    const char* name; // for menus and screen readers
};

const std::array<TrackColour, 8>& getTrackColours();

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
// Track heights in the playlist, at the normal interface size. Projects keep them, without undo steps,
// as they keep other view settings.
namespace TrackHeight
{
    inline constexpr int minimum = 32, standard = 64, maximum = 320, step = 16;
    inline constexpr int collapsed = 24; // a collapsed track's row, which shows its name
} // namespace TrackHeight

int getTrackHeight (const te::Track&);
void setTrackHeight (te::Track&, int height); // kept between the minimum and the maximum

// How tall the playlist shows a track: its height, or TrackHeight::collapsed while it is collapsed. A collapsed
// folder keeps its height, as collapsing it hides its tracks instead.
int getShownTrackHeight (const te::Track&);

// Sets how tall the playlist shows a track, as dragging its bottom edge does: well below the minimum, a track
// collapses, and above it, a collapsed track expands again. (A folder stops at the minimum.)
void setShownTrackHeight (te::Track&, int height);

// Makes a track taller or shorter by some pixels, as the height commands do, collapsing or expanding it the same way.
void resizeTrack (te::Track&, int change);

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

// The track commands that need no window: add, select the track above or below, move, mute, solo and
// height. They act on the tracks selected in the selection manager's Edit. Delete and duplicate are
// selection commands (Selection.h); the playlist adds rename and color.
void addTrackCommands (CommandRegistry&, te::SelectionManager&);
} // namespace phazite
