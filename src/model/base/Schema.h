// SPDX-License-Identifier: AGPL-3.0-or-later
// Everything Phazite writes into a project file besides Tracktion's own data: the names of its elements, properties
// and plug-in type, each defined once, here, with where it is stored and what it holds. Code reads and writes them
// only through these names. Never rename one: projects saved before would lose what it held. A change to what one
// holds needs a new schema version and a migration.
//
// Kinds: song = part of the song, changed in undo steps; view = how the project is shown, saved but never an undo
// step; state = remembered by the project, not an undo step.
//
// Write the names qualified, as schema::outline, never with "using namespace schema": they would clash with
// parameters and variables of the same name.
#pragma once

#include "Tracktion.h"

namespace phazite::schema
{
//==============================================================================
// The project format's version (src/model/base/Migrations.h). Files without schemaVersion are version 0 (saved up to
// 2026-10-14).
inline constexpr int currentVersion = 1;

//==============================================================================
// Phazite's state: what Phazite keeps for the whole song (getPhaziteState()).

// Child of EDIT, added when something is first written to it.
inline const juce::Identifier PHAZITE { "PHAZITE" };

// On PHAZITE. int: the project format's version (currentVersion) when the project was saved. (Not 'version': tests write
// that name.) state
inline const juce::Identifier schemaVersion { "schemaVersion" };

// On PHAZITE. bool, default true: the loop follows the song (getSongLoopRange()); false while a range is looped on
// purpose. Written only when it changes. state
inline const juce::Identifier loopFollowsSong { "loopFollowsSong" };

// On PHAZITE. The id of the song outline applied last (Outline::id), or none. song
inline const juce::Identifier outline { "outline" };

// Child of PHAZITE: the key set last on any track (phaziteKeyRoot and phaziteKeyScale), which new tracks get. state
inline const juce::Identifier LASTKEY { "LASTKEY" };

//==============================================================================
// A grid's settings (GridSettings): the playlist's on PHAZITE, the piano roll's on PIANOROLL. view

// bool, default true: positions snap to the grid.
inline const juce::Identifier gridSnap { "gridSnap" };

// bool, default true: the grid follows the zoom.
inline const juce::Identifier gridAdaptive { "gridAdaptive" };

// int -3..3, default 0: how many levels coarser (above 0) or finer than the zoom suggests an adaptive grid is.
inline const juce::Identifier gridOffset { "gridOffset" };

// The size of a fixed grid, used while gridAdaptive is off: "1/16 beat", "1/8 beat", "1/4 beat" (the default), "1/2 beat",
// "1 beat", "1 bar", "2 bars", "4 bars" or "8 bars". (Version 0 saved a Tracktion snap level instead: Migrations.cpp.)
inline const juce::Identifier gridStep { "gridStep" };

// bool, default false: the grid divides beats into triplets.
inline const juce::Identifier gridTriplets { "gridTriplets" };

// Child of PHAZITE: the piano roll's grid settings, added when they are first written. view
inline const juce::Identifier PIANOROLL { "PIANOROLL" };

//==============================================================================
// On tracks.

// On TRACK and FOLDERTRACK. int 32..320, default 64: the track's height in the playlist, in pixels at an interface
// scale of 100%. view
inline const juce::Identifier phaziteHeight { "phaziteHeight" };

// On TRACK and FOLDERTRACK. bool, default false: the track is a thin row with only its name; a collapsed folder hides
// its tracks. view
inline const juce::Identifier phaziteCollapsed { "phaziteCollapsed" };

// On TRACK and FOLDERTRACK. bool, default false: the track stays in view while the others scroll. view
inline const juce::Identifier phazitePinned { "phazitePinned" };

// A key, on a TRACK (song) and on LASTKEY (state): the root, int 0..11 (0 is C), none = no key; and the scale: "major",
// "minor", "dorian", "phrygian", "lydian", "mixolydian", "locrian", "harmonicMinor" or "melodicMinor". (Version 0 saved
// Tracktion's scale numbers instead.)
inline const juce::Identifier phaziteKeyRoot { "phaziteKeyRoot" };
inline const juce::Identifier phaziteKeyScale { "phaziteKeyScale" };

// On TRACK. bool: the piano roll shows the track's clips in drum mode; none = on while the track's instrument is
// Tracktion's sampler. view
inline const juce::Identifier phaziteDrumMode { "phaziteDrumMode" };

// On TRACK: the order of the drum rows, as note numbers separated by commas, top first; rows it doesn't list come
// after them. view
inline const juce::Identifier phaziteDrumOrder { "phaziteDrumOrder" };

// On TRACK: the drum rows hidden, as note numbers separated by commas. view
inline const juce::Identifier phaziteDrumHidden { "phaziteDrumHidden" };

// On FOLDERTRACK. true, or none: the folder was made a bus with the bus switch, so it stays one without effects. song
inline const juce::Identifier phaziteBus { "phaziteBus" };

//==============================================================================
// On sections: Tracktion's arranger clips (ARRANGERCLIP). song

// What the section does in the song, such as "intro", "verse" or "drop", or none.
inline const juce::Identifier phaziteRole { "phaziteRole" };

// A one-line suggestion of what to put in the section, or none.
inline const juce::Identifier phaziteTip { "phaziteTip" };

// float 0..1, default 0.5: how intense the section is, for the energy curve.
inline const juce::Identifier phaziteEnergy { "phaziteEnergy" };

//==============================================================================
// The type of Phazite's loudness meter (LoudnessMeterPlugin), a PLUGIN that every project has on its master.
inline constexpr const char* phaziteLoudnessMeter = "phaziteLoudnessMeter";
} // namespace phazite::schema

namespace phazite
{
// An Edit's Phazite state: the PHAZITE element, added if the Edit has none yet (without an undo step).
juce::ValueTree getPhaziteState (te::Edit&);

// The same, but without adding it if the Edit has none yet (adding it would count as an unsaved change): then the
// result is invalid, and reading a property from it gives the default.
juce::ValueTree findPhaziteState (const te::Edit&);
} // namespace phazite
