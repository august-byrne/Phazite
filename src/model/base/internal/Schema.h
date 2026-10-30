// SPDX-License-Identifier: AGPL-3.0-or-later
// Everything Phazite writes into a project file besides Tracktion's own data: the names of its elements, properties
// and plug-in type, each defined once, here, with where it is stored and what it holds. Code reads and writes them
// only through these names.
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
// Phazite's state: what Phazite keeps for the whole song (getPhaziteState()).

// Child of EDIT. Every project has one (Project adds it as the project opens); another Edit gets one when something is
// first written to it.
inline const juce::Identifier PHAZITE { "PHAZITE" };

// On PHAZITE. bool, default true: the loop follows the song (getSongLoopRange()); false while a range is looped on
// purpose. Written only when it changes. state
inline const juce::Identifier loopFollowsSong { "loopFollowsSong" };

// On PHAZITE. The id of the song outline applied last (Outline::id), or none. song
inline const juce::Identifier outline { "outline" };

// On PHAZITE. bool, default false: a change of the song's tempo leaves its patterns (MIDI clips) at their speed, their
// notes keeping their times, rather than on their beats (model/transport/internal/Tempo.h). song
inline const juce::Identifier patternsKeepTheirSpeed { "patternsKeepTheirSpeed" };

// Child of PHAZITE: the key set last on any track (phaziteKeyRoot and phaziteKeyScale), which new tracks get. state
inline const juce::Identifier LASTKEY { "LASTKEY" };

//==============================================================================
// The song's tempo map (model/transport/internal/Tempo.h; docs/tracktion-migration.md, T1): its tempos and time signatures,
// each from a beat on, which everything in the song is placed on. Tracktion's tempo sequence is a mirror of it
// (model/mirror/internal/Mirror.h). For now a song has one tempo and one time signature, from its start; a tempo lane will
// add changes along it. song

// Child of PHAZITE, which every project has (Project adds it as the project opens): the song's tempos (TEMPO) and time
// signatures (TIMESIGNATURE), in no particular order. Without one, the song is at 120 BPM in 4/4.
inline const juce::Identifier TEMPOMAP { "TEMPOMAP" };

// Child of TEMPOMAP: a tempo, from where it starts (mapBeat) to the next one's start, in beats per minute (bpm, 20..300,
// to a hundredth). The first starts at 0.
inline const juce::Identifier TEMPO { "TEMPO" };
inline const juce::Identifier bpm { "bpm" };

// Child of TEMPOMAP: a time signature, from where it starts (mapBeat), as many beats in a bar (numerator, int 1..32) of a
// note value (denominator: 2, 4, 8 or 16). The first starts at 0.
inline const juce::Identifier TIMESIGNATURE { "TIMESIGNATURE" };
inline const juce::Identifier numerator { "numerator" };
inline const juce::Identifier denominator { "denominator" };

// On TEMPO and TIMESIGNATURE. double, 0 or more: where it starts, in beats from the song's start.
inline const juce::Identifier mapBeat { "beat" };

//==============================================================================
// A grid's settings (GridSettings): the playlist's on PHAZITE, the piano roll's on PIANOROLL. view

// bool, default true: positions snap to the grid.
inline const juce::Identifier gridSnap { "gridSnap" };

// bool, default true: the grid follows the zoom.
inline const juce::Identifier gridAdaptive { "gridAdaptive" };

// int -3..3, default 0: how many levels coarser (above 0) or finer than the zoom suggests an adaptive grid is.
inline const juce::Identifier gridOffset { "gridOffset" };

// The size of a fixed grid, used while gridAdaptive is off: "1/16 beat", "1/8 beat", "1/4 beat" (the default), "1/2 beat",
// "1 beat", "1 bar", "2 bars", "4 bars" or "8 bars".
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
// "minor", "dorian", "phrygian", "lydian", "mixolydian", "locrian", "harmonicMinor" or "melodicMinor".
inline const juce::Identifier phaziteKeyRoot { "phaziteKeyRoot" };
inline const juce::Identifier phaziteKeyScale { "phaziteKeyScale" };

// On TRACK. bool: the piano roll shows the track's clips in drum mode; none = on while the track's instrument is
// Phazite's drum sampler. view
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
// here shows one after these. view
inline const juce::Identifier PHAZITELANE { "PHAZITELANE" };

// On PHAZITELANE: the device, by the EditItemID Tracktion saves it with ("1042").
inline const juce::Identifier laneDevice { "device" };

// On PHAZITELANE: the parameter, by its ID in its device (the paramID of its automation curve in Tracktion's AUTOMATIONCURVE).
inline const juce::Identifier laneParameter { "parameter" };

// On PHAZITELANE. int 32..320, default 48: the lane's height in the playlist while it isn't collapsed, in pixels at an
// interface scale of 100%.
inline const juce::Identifier laneHeight { "height" };

// On PHAZITELANE. true, or none: the lane shows as a thin row.
inline const juce::Identifier laneCollapsed { "collapsed" };

// View settings: saved, written without undo steps, and not unsaved changes (decision 4; Project's isUncountedProperty()
// and isUncountedElement()): on tracks, their heights, collapsed and pinned, drum mode and rows, and their automation
// lanes; on PHAZITE, the playlist's grid; on devices (PLUGIN), where their windows were. PIANOROLL and PHAZITELANE hold
// view settings only. Keep these lists exact: anything else a project holds is song or state.
inline bool isTrackViewProperty (const juce::Identifier& p)
{
    return p == phaziteHeight || p == phaziteCollapsed || p == phazitePinned || p == phaziteDrumMode || p == phaziteDrumOrder
           || p == phaziteDrumHidden;
}

inline bool isGridProperty (const juce::Identifier& p)
{
    return p == gridSnap || p == gridAdaptive || p == gridOffset || p == gridStep || p == gridTriplets;
}

// Tracktion's names, which Phazite writes as view settings, without the undo manager (model/mixer/internal/Mixer.h:
// rememberWindowPosition(), and model/devices/internal/DeviceStates.h as it saves).
inline bool isDeviceViewProperty (const juce::Identifier& p)
{
    return p == te::IDs::windowX || p == te::IDs::windowY;
}

inline bool isTrackViewElement (const juce::ValueTree& element)
{
    return element.hasType (PHAZITELANE);
}

//==============================================================================
// The song's sections (model/arrangement/internal/Sections.h; docs/design.md, section 6): named, colored parts of the
// song over every track. song

// Child of PHAZITE, added with the first section: the sections (SECTION), in no particular order.
inline const juce::Identifier SECTIONS { "SECTIONS" };

// Child of SECTIONS: a section. Sections don't overlap.
inline const juce::Identifier SECTION { "SECTION" };

// On SECTION. int64, more than 0: its ID (SectionID), which the Edit gives it as it gives its tracks and clips theirs, so
// that no other item of the project has it. Tracktion's name for an item's ID, so that Tracktion counts it too.
inline const juce::Identifier sectionID { "id" };

// On SECTION. double, 0 or more: where it starts, in beats from the song's start.
inline const juce::Identifier sectionStart { "start" };

// On SECTION. double, more than 0: how long it lasts, in beats.
inline const juce::Identifier sectionLength { "length" };

// On SECTION: its name.
inline const juce::Identifier sectionName { "name" };

// On SECTION: what it does in the song, such as "intro", "verse" or "drop", or none.
inline const juce::Identifier sectionRole { "role" };

// On SECTION: a one-line suggestion of what to put in it, or none.
inline const juce::Identifier sectionTip { "tip" };

// On SECTION. double 0..1, default 0.5: how intense it is, for the energy curve.
inline const juce::Identifier sectionEnergy { "energy" };

// On SECTION, if it was given a color of its own: the color, as juce::Colour::toString() writes it. Without it, its
// role's.
inline const juce::Identifier sectionColour { "colour" };

//==============================================================================
// The chord lane (model/chords/ChordModel.h; docs/design.md, section 14): the song's chords, each a Roman numeral in the
// key there, so that the chords follow when the key changes. song

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
// On audio clips (Tracktion's AUDIOCLIP) whose sound Phazite renders itself (model/arrangement/internal/AudioClips.h;
// roadmap step 2.18): one that follows the song's tempo, or is pitched or reversed. Tracktion plays such a clip as a
// plain clip of the render (its source, a file in the Edit's temporary folder, the project's Cache folder), and these
// say what the render is made from. A clip without phaziteSource plays its source as it is. song

// The audio file the clip's sound comes from, as Tracktion names a clip's source: a path relative to the project's
// file, or a full path.
inline const juce::Identifier phaziteSource { "phaziteSource" };

// double 20..300 (AudioClipModel.h: minClipTempo..maxClipTempo), or none: the tempo the file was played at, in beats
// per minute, while the clip stretches to follow the song's; none while it plays at the file's own speed.
inline const juce::Identifier phaziteTempo { "phaziteTempo" };

// float -48..48, default 0: how many semitones the clip is pitched up or down, with cents as hundredths.
inline const juce::Identifier phazitePitch { "phazitePitch" };

// bool, default false: the clip plays its file backwards.
inline const juce::Identifier phaziteReversed { "phaziteReversed" };

//==============================================================================
// Recording audio (model/transport/RecordingModel.h; roadmap step 3.4).

// On TRACK. The audio device's input the track records (AudioInput::id): a channel, counted from 1 ("1"), or a pair of
// neighbours from an odd-numbered one ("1+2"); none for no input. song
inline const juce::Identifier phaziteInput { "phaziteInput" };

// On TRACK. When the track's input is heard through it: monitorAlways, monitorNever, or none, while it is armed. song
inline const juce::Identifier phaziteMonitoring { "phaziteMonitoring" };
inline constexpr const char* monitorAlways = "always";
inline constexpr const char* monitorNever = "never";

// On PHAZITE. The input chosen last on any track (an id as phaziteInput's), which the next track's list shows first. state
inline const juce::Identifier lastInput { "lastInput" };

// On PHAZITE. bool, default false: Record punches in and out at the looped range. Like the loop's switch, not an unsaved
// change. state
inline const juce::Identifier punch { "punch" };

// On AUDIOCLIP: a clip recorded round a looped range, whose file holds a take for each time round, one after another
// (ClipTakes). song
// int, 2 or more: how many takes the file holds.
inline const juce::Identifier phaziteTakes { "phaziteTakes" };

// int, 1..phaziteTakes: the take the clip plays.
inline const juce::Identifier phaziteTake { "phaziteTake" };

// double, more than 0: how long each take is, in seconds: the looped range's length when it was recorded.
inline const juce::Identifier phaziteTakeLength { "phaziteTakeLength" };

//==============================================================================
// On notes: Tracktion's NOTEs, in a MIDI clip's SEQUENCE. song

// int, 1 or more: the note's ID (NoteID, model/base/IDs.h), which no other note of its clip has, so that selections, undo
// and scripts can name one note (decided 2026-10-14). A copy of a clip, an alias too, keeps its notes' IDs. Each note
// gets one when it is added (model/notes/internal/Notes.h: NoteIDKeeper), and any that has none when a project opens or is saved
// (assignNoteIDs()).
inline const juce::Identifier phaziteNoteID { "phaziteNoteID" };

//==============================================================================
// The type of Phazite's loudness meter (LoudnessMeterPlugin), a PLUGIN that every project has on its master.
inline constexpr const char* phaziteLoudnessMeter = "phaziteLoudnessMeter";

// Phazite's own devices (model/devices/internal/Devices.h) are PLUGINs too. Each one's type is the typeID of its description
// (src/devices/<area>/: "phaziteEq" for the EQ), and its parameters' values are properties of the PLUGIN named by their
// IDs in that description, as are its automation curves' paramIDs. So no parameter's ID may be a property Tracktion keeps
// on every PLUGIN (type, id, enabled, process...): its value would take that property's place.

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

// Phazite's wavetable synth (devices/wavetable/WavetableSynth.h) also keeps, on its PLUGIN, what each of its two
// oscillators plays: "osc1Wavetable" and "osc2Wavetable" name one of its factory tables (devices/wavetable/
// FactoryWavetables.h: "Vowels"; with none, or a name that is none of theirs, the first, "Basic Shapes"), and
// "osc1WavetableFile" and "osc2WavetableFile" a sound file it plays instead while they are there: a path relative to the
// project's folder ("Audio/Growl.wav"), or in a preset its full path. osc counts from 0.
inline const juce::Identifier& getOscillatorWavetable (int osc)
{
    static const juce::Identifier names[] = { "osc1Wavetable", "osc2Wavetable" };
    return names[juce::jlimit (0, 1, osc)];
}

inline const juce::Identifier& getOscillatorWavetableFile (int osc)
{
    static const juce::Identifier names[] = { "osc1WavetableFile", "osc2WavetableFile" };
    return names[juce::jlimit (0, 1, osc)];
}

// On a plug-in's PLUGIN (Tracktion's "vst" type, whatever the plug-in's format): each parameter the project refers to (its
// automation, what modulates it, its value set by hand under automation, its automation lane), as the ID Tracktion knows
// it by, its place in the plug-in's list, then "=" and the plug-in's own ID for it (a VST3's), which stays the same
// through the plug-in's updates; separated by spaces ("3=1084 7=2001"). Or none. A plug-in that opens moves what refers to
// a parameter that its update moved (model/devices/internal/PluginParameters.h). state
inline const juce::Identifier phaziteParameterIDs { "phaziteParameterIDs" };

//==============================================================================
// Presets (model/presets/PresetModel.h): files of their own (".phazitepreset"), outside any project, in JSON, in Phazite's
// own names: devices by the type and parameter IDs of their descriptions (src/devices/), plug-ins by who they are and the
// settings they give themselves, and containers, modulators and macros as Phazite names them. The model makes Tracktion's
// objects from them as a preset loads (roadmap A5, 2026-10-30); nothing of Tracktion's is in them. No format version
// (section 18 of docs/design.md: none while Phazite is pre-alpha).
//
//   { "kind": "device", "device": DEVICE }            an instrument or an effect
//   { "kind": "container", "container": CONTAINER }   a container of effects
//   { "kind": "track", "track": TRACK }               a track's devices, modulators and mix
//
//   DEVICE, one of Phazite's own: { "type": its type ID, "name", "instrument": true or false,
//       "parameters": { parameter ID: value, ... }, every one of them, its mix too,
//       "sounds": { what names a file or a table (getPadSound(), getOscillatorWavetable(), getOscillatorWavetableFile()):
//                   a file's full path, or a factory table's name } }
//     or a plug-in: { "plugin": { "format", "name", "manufacturer", "identifier", "uid" }, "name", "instrument",
//       "state": its own settings, in base 64, "program": its program, "mix": 0 to 1 }
//   CONTAINER: { "name", "mix": 0 to 1, "devices": [ DEVICE, ... ], "macros": [ MACRO, ... ] }
//   MACRO: { "name", "value": 0 to 1, "modulates": [ TARGET, ... ] }
//   TRACK: { "name", "colour": its ARGB in hex, "volume": in decibels, "pan": -1 to 1, "drumMode", "drumOrder",
//            "drumHidden" (its drum rows' view state, as the project keeps them), "instrument": DEVICE,
//            "effects": [ DEVICE or { "container": CONTAINER }, ... ], "modulators": [ MODULATOR, ... ],
//            "macros": [ MACRO, ... ] }
//   MODULATOR: { "type": one of modulatorTypeNames, "parameters": { parameter ID: value, ... },
//                "steps": [ value, ... ] (a step sequencer's), "modulates": [ TARGET, ... ] }
//   TARGET: what a modulator or a macro modulates, by an amount from -1 to 1: { "parameter": ID, "amount", and where:
//           a container's macro's, "device": its index among the container's devices; a track's, "instrument": true,
//           or "effect": its index among the track's effects (with "inside": its index in that container, for a device
//           in one), or "mixer": true with "parameter" "volume" or "pan" }
namespace preset
{
    inline const juce::Identifier kind { "kind" }, device { "device" }, container { "container" }, track { "track" };
    inline const juce::Identifier type { "type" }, name { "name" }, instrument { "instrument" }, parameters { "parameters" },
        sounds { "sounds" };
    inline const juce::Identifier plugin { "plugin" }, format { "format" }, manufacturer { "manufacturer" }, identifier { "identifier" },
        uid { "uid" }, state { "state" }, program { "program" }, mix { "mix" };
    inline const juce::Identifier devices { "devices" }, macros { "macros" }, value { "value" }, modulates { "modulates" };
    inline const juce::Identifier parameter { "parameter" }, amount { "amount" }, effect { "effect" }, inside { "inside" },
        mixer { "mixer" };
    inline const juce::Identifier colour { "colour" }, volume { "volume" }, pan { "pan" }, drumMode { "drumMode" },
        drumOrder { "drumOrder" }, drumHidden { "drumHidden" }, effects { "effects" }, modulators { "modulators" }, steps { "steps" };

    // What "kind" says.
    inline constexpr const char* deviceKind = "device";
    inline constexpr const char* containerKind = "container";
    inline constexpr const char* trackKind = "track";

    // What a modulator's "type" says, in ModulatorType's order (model/modulation/ModulationValues.h), macros left out.
    inline constexpr const char* modulatorTypeNames[] = { "lfo", "envelopeFollower", "steps", "random", "breakpoints", "noteFollower" };
} // namespace preset
} // namespace phazite::schema

namespace phazite
{
// An Edit's Phazite state: the PHAZITE element, added if the Edit has none yet (without an undo step).
juce::ValueTree getPhaziteState (te::Edit&);

// The same, but without adding it if the Edit has none yet (adding it would count as an unsaved change): then the
// result is invalid, and reading a property from it gives the default.
juce::ValueTree findPhaziteState (const te::Edit&);
} // namespace phazite
