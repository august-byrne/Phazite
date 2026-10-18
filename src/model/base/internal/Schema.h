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

#include "model/base/internal/Tracktion.h"

namespace phazite::schema
{
//==============================================================================
// The project format's version (src/model/base/Migrations.h). Files without schemaVersion are version 0 (saved up to
// 2026-10-14); version 1 saves the grid's size and keys' scales by name; version 2 can hold Phazite's own devices
// (roadmap step 1.6), which an older Phazite would drop; version 3 gives every note an ID (phaziteNoteID); version 4 adds
// automation lanes (PHAZITELANE, roadmap step 2.1); version 5 has no automation clips (step 2.2 until 2026-10-17): their
// curves are their parameters' own automation; version 6 lets lanes collapse (laneCollapsed); version 7 adds the chord
// lane (CHORDLANE, step 2.7) and parts made from it (phaziteChordPart); version 8 can hold the devices of step 2.10 (the
// delay, saturator, filter, utility, multiband dynamics and maximizer) and the synth's unison, which an older Phazite would
// drop or play without.
inline constexpr int currentVersion = 8;

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

// Child of TRACK, FOLDERTRACK and MASTERTRACK, one for each automation lane the playlist shows under the track, in their
// order (docs/design.md, section 10): the automation of a parameter of one of the track's devices, its volume and pan
// included. A lane of a device that isn't on the track any more is ignored, and a parameter with automation but no lane
// here shows one after these. Version 4 added them. view
inline const juce::Identifier PHAZITELANE { "PHAZITELANE" };

// On PHAZITELANE: the device, by the EditItemID Tracktion saves it with ("1042").
inline const juce::Identifier laneDevice { "device" };

// On PHAZITELANE: the parameter, by its ID in its device (the paramID of its automation curve in Tracktion's AUTOMATIONCURVE).
inline const juce::Identifier laneParameter { "parameter" };

// On PHAZITELANE. int 32..320 (24..320 up to version 5), default 48: the lane's height in the playlist while it isn't
// collapsed, in pixels at an interface scale of 100%.
inline const juce::Identifier laneHeight { "height" };

// On PHAZITELANE. true, or none: the lane shows as a thin row. Version 6 added it.
inline const juce::Identifier laneCollapsed { "collapsed" };

// View settings: saved, written without undo steps, and not unsaved changes (decision 4; Project's isUncountedProperty()
// and isUncountedElement()): on tracks, their heights, collapsed and pinned, drum mode and rows, and their automation
// lanes; on PHAZITE, the playlist's grid. PIANOROLL and PHAZITELANE hold view settings only. Keep these lists exact:
// anything else a project holds is song or state.
inline bool isTrackViewProperty (const juce::Identifier& p)
{
    return p == phaziteHeight || p == phaziteCollapsed || p == phazitePinned || p == phaziteDrumMode || p == phaziteDrumOrder
           || p == phaziteDrumHidden;
}

inline bool isGridProperty (const juce::Identifier& p)
{
    return p == gridSnap || p == gridAdaptive || p == gridOffset || p == gridStep || p == gridTriplets;
}

inline bool isTrackViewElement (const juce::ValueTree& element)
{
    return element.hasType (PHAZITELANE);
}

//==============================================================================
// On sections: Tracktion's arranger clips (ARRANGERCLIP). song

// What the section does in the song, such as "intro", "verse" or "drop", or none.
inline const juce::Identifier phaziteRole { "phaziteRole" };

// A one-line suggestion of what to put in the section, or none.
inline const juce::Identifier phaziteTip { "phaziteTip" };

// float 0..1, default 0.5: how intense the section is, for the energy curve.
inline const juce::Identifier phaziteEnergy { "phaziteEnergy" };

//==============================================================================
// The chord lane (model/chords/ChordModel.h; docs/design.md, section 14): the song's chords, each a Roman numeral in the
// key there, so that the chords follow when the key changes. Version 7 added it. song

// Child of PHAZITE, added when the first chord or key is written: the chords (CHORD) and the keys they are in (CHORDKEY),
// in no particular order.
inline const juce::Identifier CHORDLANE { "CHORDLANE" };

// Child of CHORDLANE: a chord over a stretch of the song. Chords don't overlap.
inline const juce::Identifier CHORD { "CHORD" };

// On CHORD and CHORDKEY. double, 0 or more: where it starts, in beats from the song's start.
inline const juce::Identifier chordStart { "start" };

// On CHORD. double, more than 0: how long it lasts, in beats.
inline const juce::Identifier chordLength { "length" };

// On CHORD. int 1..7: the step of the key's scale its root is on (Numeral::degree in model/chords/ChordValues.h).
inline const juce::Identifier chordDegree { "degree" };

// On CHORD. int -1, 0 or 1, default 0: the root a semitone under (-1) or over (1) that step, as in "bVII".
inline const juce::Identifier chordAccidental { "accidental" };

// On CHORD: its kind, "major", "minor", "dominantSeventh", "majorSeventh", "minorSeventh", "suspendedSecond",
// "suspendedFourth", "diminished", "augmented", "majorSixth", "minorSixth", "dominantNinth", "majorNinth", "power",
// "halfDiminishedSeventh", "diminishedSeventh" or "minorNinth" (getSavedChordTypeName()).
inline const juce::Identifier chordType { "type" };

// On CHORD. int, default 0: which of its notes is lowest, 0 for its root, 1 for the next one up, and so on.
inline const juce::Identifier chordInversion { "inversion" };

// Child of CHORDLANE: the key the chords are in from where it starts (chordStart) to the next one's start, with
// phaziteKeyRoot and phaziteKeyScale. The first starts at 0; a lane without any is in C major.
inline const juce::Identifier CHORDKEY { "CHORDKEY" };

// On a MIDICLIP made from the chord lane: the part it is, "sustainedTriads", "sustainedSevenths", "spreadChords",
// "pulsingChords", "offbeatStabs", "bassRoots", "bassPulses", "bassRootFifth", "bassOctaves", "arpeggioUp",
// "arpeggioDown" or "arpeggioUpDown" (getSavedPartStyleName()). While its notes are the ones the lane makes for it, they
// change with the chords. song
inline const juce::Identifier phaziteChordPart { "phaziteChordPart" };

//==============================================================================
// On notes: Tracktion's NOTEs, in a MIDI clip's SEQUENCE. song

// int, 1 or more: the note's ID (NoteID, model/base/IDs.h), which no other note of its clip has, so that selections, undo
// and scripts can name one note (decided 2026-10-14). A copy of a clip, an alias too, keeps its notes' IDs. Each note
// gets one when it is added (model/notes/internal/Notes.h: NoteIDKeeper), and any that has none when a project opens or is saved
// (assignNoteIDs()). Version 3 added them.
inline const juce::Identifier phaziteNoteID { "phaziteNoteID" };

//==============================================================================
// The type of Phazite's loudness meter (LoudnessMeterPlugin), a PLUGIN that every project has on its master.
inline constexpr const char* phaziteLoudnessMeter = "phaziteLoudnessMeter";

// Phazite's own devices (model/devices/internal/Devices.h) are PLUGINs too. Each one's type is the typeID of its description
// (src/devices/<area>/: "phaziteEq" for the EQ), and its parameters' values are properties of the PLUGIN named by their
// IDs in that description, as are its automation curves' paramIDs. Those IDs are part of the format: they never
// change, and changing what one holds needs a new version and a migration, as anything here does.

// Phazite's drum sampler (devices/drums/DrumSampler.h) also keeps, on its PLUGIN, the sound file each pad plays: a path
// relative to the project's folder ("Audio/Kick 01.wav"), or none (no property) for the default kit's sound; in a preset,
// its full path. These are "pad1Sound" for its first pad up to "pad16Sound"; pad counts from 0.
inline const juce::Identifier& getPadSound (int pad)
{
    static const juce::Identifier names[] = { "pad1Sound",  "pad2Sound",  "pad3Sound",  "pad4Sound",  "pad5Sound",  "pad6Sound",
                                              "pad7Sound",  "pad8Sound",  "pad9Sound",  "pad10Sound", "pad11Sound", "pad12Sound",
                                              "pad13Sound", "pad14Sound", "pad15Sound", "pad16Sound" };
    return names[juce::jlimit (0, 15, pad)];
}

//==============================================================================
// Presets (model/presets/PresetModel.h): files of their own (".phazitepreset"), outside any project, that hold a device,
// a container or a track as a project holds them (Tracktion's elements with Phazite's names above), in the project format
// of the version that saved them, which is brought up to date as a project's is (Migrations.h).

// The element at a preset file's root. Its children: a device preset's PLUGIN; a container preset's PLUGIN (a rack) and
// the RACK it plays; a track preset's TRACK, with the RACKs its containers play.
inline const juce::Identifier PHAZITEPRESET { "PHAZITEPRESET" };

// On PHAZITEPRESET: "device", "container" or "track".
inline const juce::Identifier presetKind { "kind" };

// On PHAZITEPRESET: a device preset's device as the browser names it ("EQ", or a plug-in's name).
inline const juce::Identifier presetDevice { "device" };

// On PHAZITEPRESET. bool, default false: a device preset's device is an instrument.
inline const juce::Identifier presetInstrument { "instrument" };

// On PHAZITEPRESET, as on PHAZITE: the project format's version (currentVersion) of what it holds.
// (schemaVersion, above)
} // namespace phazite::schema

namespace phazite
{
// An Edit's Phazite state: the PHAZITE element, added if the Edit has none yet (without an undo step).
juce::ValueTree getPhaziteState (te::Edit&);

// The same, but without adding it if the Edit has none yet (adding it would count as an unsaved change): then the
// result is invalid, and reading a property from it gives the default.
juce::ValueTree findPhaziteState (const te::Edit&);
} // namespace phazite
