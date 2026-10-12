// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist's tracks: which ones it shows, their colors and heights, and the track commands.
#pragma once

#include "Commands.h"

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
}

int getTrackHeight (const te::Track&);
void setTrackHeight (te::Track&, int height); // kept between the minimum and the maximum

//==============================================================================
// Inserts an empty track after another one (at the end if that is nullptr), in the palette's next
// color, and selects it alone. Returns nullptr if the Edit already has as many tracks as it can hold.
te::AudioTrack* insertTrack (te::Edit&, te::Track* after, te::SelectionManager*);

// The track commands that need no window: add, select the track above or below, move, mute, solo and
// height. They act on the tracks selected in the selection manager's Edit. Delete and duplicate are
// selection commands (Selection.h); the playlist adds rename and color.
void addTrackCommands (CommandRegistry&, te::SelectionManager&);
} // namespace phazite
