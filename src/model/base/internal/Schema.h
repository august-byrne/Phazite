// SPDX-License-Identifier: AGPL-3.0-or-later
// Everything Phazite writes into a project file: the names of its elements, properties and values, each defined once,
// here, with where it is stored and what it holds. Code reads and writes them only through these names.
//
// Kinds: song = part of the song, changed in undo steps; view = how the project is shown, saved but never an undo
// step; state = remembered by the project, not an undo step.
//
// Write the names qualified, as schema::outline, never with "using namespace schema": they would clash with
// parameters and variables of the same name.
//
// Each name is defined by its description (SchemaDescription.h): an element type (Element) with the elements it goes in, or
// a property (Property<T>) with the elements it is on, the C++ type of its value, and what its comment says of it as facts
// (its range, its words, its default, its kind, whether it counts as a change). The loader (model/project/internal/
// Project.h: makeConsistent()), the consistency check (tests/SongCheck.h) and the rules below read the descriptions.
#pragma once

#include "model/base/Song.h"
#include "model/base/internal/SchemaDescription.h"

#include <algorithm>
#include <array>
#include <map>
#include <vector>

namespace phazite::schema
{
//==============================================================================
// The song's elements, each with the elements it goes in ("Child of" below) and its kind, described here before the
// properties that name them; each is explained below, where what it holds is.
// clang-format off
inline const Element PHAZITE       { "PHAZITE", Kind::song };
inline const Element LASTKEY       { "LASTKEY", under (PHAZITE), Kind::state };
inline const Element TEMPOMAP      { "TEMPOMAP", under (PHAZITE), Kind::song };
inline const Element TEMPO         { "TEMPO", under (TEMPOMAP), Kind::song };
inline const Element TIMESIGNATURE { "TIMESIGNATURE", under (TEMPOMAP), Kind::song };
inline const Element PIANOROLL     { "PIANOROLL", under (PHAZITE), Kind::view, uncounted };
inline const Element TRACKS        { "TRACKS", under (PHAZITE), Kind::song };
inline const Element TRACK         { "TRACK", under (TRACKS, TRACK), Kind::song };
inline const Element MASTER        { "MASTER", under (PHAZITE), Kind::song };
inline const Element LANES         { "LANES", under (TRACK, MASTER), Kind::view, uncounted };
inline const Element LANE          { "LANE", under (LANES), Kind::view };
inline const Element CONTAINER     { "CONTAINER", under (TRACK, MASTER), Kind::song };
inline const Element CHAIN         { "CHAIN", under (CONTAINER), Kind::song };
inline const Element DEVICE        { "DEVICE", under (TRACK, MASTER, CHAIN), Kind::song };
inline const Element MODULATOR     { "MODULATOR", under (TRACK, MASTER), Kind::song };
inline const Element VALUES        { "VALUES", under (DEVICE, MODULATOR), Kind::song, namedByParameters };
inline const Element SOUNDS        { "SOUNDS", under (DEVICE), Kind::song };
inline const Element VALUE         { "VALUE", under (DEVICE), Kind::song };
inline const Element SEND          { "SEND", under (TRACK), Kind::song };
inline const Element MACRO         { "MACRO", under (TRACK, CONTAINER), Kind::song };
inline const Element MODULATES     { "MODULATES", under (MODULATOR, MACRO), Kind::song };
inline const Element AUTOMATION    { "AUTOMATION", under (PHAZITE), Kind::song };
inline const Element CURVE         { "CURVE", under (AUTOMATION), Kind::song };
inline const Element POINT         { "POINT", under (CURVE), Kind::song };
inline const Element MIDILINKS     { "MIDILINKS", under (PHAZITE), Kind::song };
inline const Element MIDILINK      { "MIDILINK", under (MIDILINKS), Kind::song };
inline const Element MERGING       { "MERGING", under (PHAZITE), Kind::song };
inline const Element SECTIONS      { "SECTIONS", under (PHAZITE), Kind::song };
inline const Element SECTION       { "SECTION", under (SECTIONS), Kind::song };
inline const Element CHORDLANE     { "CHORDLANE", under (PHAZITE), Kind::song };
inline const Element CHORD         { "CHORD", under (CHORDLANE), Kind::song };
inline const Element CHORDKEY      { "CHORDKEY", under (CHORDLANE), Kind::song };
inline const Element CLIP          { "CLIP", under (TRACK), Kind::song };
inline const Element NOTE          { "NOTE", under (CLIP), Kind::song };
inline const Element CONTROL       { "CONTROL", under (CLIP), Kind::song };
inline const Element WARP          { "WARP", under (CLIP), Kind::song };
// clang-format on

// The furthest place a song holds anything at, in beats (a million, days of music at any tempo) and in seconds of a
// file; and the least a length that must be more than 0 is.
inline constexpr double maxBeats = 1.0e6, maxSeconds = 1.0e7, moreThanZero = 1.0e-9;

//==============================================================================
// The song's own element, and what it keeps for the whole song (Song::getState()).

// The song (model/base/Song.h): the element a project file holds, with everything below in it. song
// (PHAZITE, with the elements above.)

// On PHAZITE. int64, above every ID in the song: the next ID it gives an item (Song::makeID()), each kind from the
// same numbers, never one twice. Written without an undo step, and no change of its own (the item it numbers is). state
inline const Property<std::int64_t> nextID { "nextID", on (PHAZITE), Kind::state, uncounted };

// On PHAZITE. Text, 16 lowercase hexadecimal digits (idNumberingDigits): the numbering the song's IDs are in, none until the
// loader first gives them anew (renumberMark below; model/project/internal/Project.h: makeConsistent()), and then a stamp
// it makes each time it does, from the song as it was given them (the first 16 digits of the SHA-256 of its project text
// then), so that one file given its IDs anew gets one stamp at every opening, and two songs given them anew differently get
// two. A version's song given its IDs anew as the store reads it (one saved in the session that crossed the mark) may get
// another stamp than the project file's, since the names of where the song stands in its versions are taken out of it
// before, and a piece that can't be read is read as a placeholder: that only makes for one refusal more, as Restore and
// Merge refuse a song given its IDs anew as it was read anyway. A stamp, not a count: two lines of one history (two
// branches, two computers sharing a folder) can each give their IDs anew once, differently. Text of any other form names
// nothing, and the loader leaves it
// out. Part of the song, which goes wherever the song goes, a version's song and a Local History snapshot too, so that
// the versions match a version's items with the song's by ID only where the two are numbered alike (areNumberedAlike()):
// Restore of a line and Merge refuse the rest. Written by the loader without an undo step, as it gives the IDs anew; else
// only as the song is made another whole, with it: in Restore This Version's undo step, and outside the undo history
// where a version is opened or a branch switched to. song
inline constexpr int idNumberingDigits = 16;
inline const Property<juce::String> idNumbering { "idNumbering", on (PHAZITE), hexDigits (idNumberingDigits), Kind::song };

// On PHAZITE. bool, default true: the loop follows the song (getSongLoopRange()); false while a range is looped on
// purpose. Written only when it changes. state
inline const Property<bool> loopFollowsSong { "loopFollowsSong", on (PHAZITE), byDefault (true), Kind::state, uncounted };

// The transport's state that the song keeps (model/transport/internal/Playback.h, Metronome.h). None is an undo step.
// On PHAZITE:
// - loop: bool, default false: playback loops, around the song while the loop follows it, or else around the range
//   looped on purpose, loopStart to loopEnd (double, in beats from the song's start; none while the loop follows the song);
// - playStart: double, in beats, default 0: where playback starts, the song's start again as a project opens;
// - metronome: bool, default false: the metronome clicks while the song plays. state
inline const Property<bool> loop { "loop", on (PHAZITE), byDefault (false), Kind::state, uncounted };
inline const Property<double> loopStart { "loopStart", on (PHAZITE), within (0.0, maxBeats), Kind::state, uncounted };
inline const Property<double> loopEnd { "loopEnd", on (PHAZITE), within (0.0, maxBeats), Kind::state, uncounted };
inline const Property<double> playStart { "playStart", on (PHAZITE), byDefault (0.0), Kind::state, uncounted };
inline const Property<bool> metronome { "metronome", on (PHAZITE), byDefault (false), Kind::state, uncounted };

// On PHAZITE. The id of the song outline applied last (Outline::id), or none. song
inline const Property<juce::String> outline { "outline", on (PHAZITE), Kind::song };

// On PHAZITE. bool, default false: a change of the song's tempo leaves its patterns (MIDI clips) at their speed, their
// notes keeping their times, rather than on their beats (model/transport/internal/Tempo.h). song
inline const Property<bool> patternsKeepTheirSpeed { "patternsKeepTheirSpeed", on (PHAZITE), byDefault (false), Kind::song };

// On PHAZITE. The project's notes, as the Project view shows them (model/project/ProjectContents.h): text, its lines
// split by "\n"; none while there are none. song
inline const Property<juce::String> notes { "notes", on (PHAZITE), Kind::song };

// Child of PHAZITE: the key set last on any track (keyRoot and keyScale), which new tracks get. state
// (LASTKEY, with the elements above.)

//==============================================================================
// The song's tempo map (model/transport/internal/Tempo.h): its tempos and time signatures, each from a beat on, which
// everything in the song is placed on. Everything in the song counts beats, quarter notes whatever the time signature, and a
// tempo counts them a minute; a time signature sets only how long a bar is and which note its own beats count
// (model/transport/TempoValues.h). The playlist's tempo lane adds tempos along the song, each held until the next or
// ramping to it; for now a song has one time signature, from its start. song

// Child of PHAZITE, which every project has (Project adds it as the project opens): the song's tempos (TEMPO) and time
// signatures (TIMESIGNATURE), read in any order, each kind sorted by beat, one of a kind for a beat as a song opens (an
// entry's key is its beat: of two at one beat the loader keeps the first; the open song can come to hold two at one beat,
// known bug 240 in docs/known-bugs.md), and written tempos first, then time signatures, each kind by beat. Without one,
// the song is at 120 BPM in 4/4. A song's tempo map is a value beside its tree while it is open
// (model/base/internal/TempoMapValue.h), each property below as its description reads it, at least one of each kind, and
// its tree's TEMPOMAP an empty place for it: so the song keeps no property or element Phazite doesn't know on a TEMPOMAP,
// a TEMPO or a TIMESIGNATURE, and writes a tempo's curve only while it isn't 1.
// (TEMPOMAP, with the elements above.)

// Child of TEMPOMAP: a tempo, from where it starts (mapBeat) to the next one's start, in quarter notes per minute (bpm, 20..300,
// to a hundredth), and how it goes on to the next (tempoCurve, float -1..1, as an automation point's pointCurve; without
// it, 1: held until the next tempo, which it jumps to; 0 ramps straight to the next, and between, the ramp bends). The
// first starts at 0.
// (TEMPO, with the elements above.)
inline const Property<double> bpm { "bpm", on (TEMPO), within (20.0, 300.0).orElse (120.0), Kind::song };
inline const Property<float> tempoCurve { "curve", on (TEMPO), within (-1.0, 1.0), byDefault (1.0f), Kind::song };

// Child of TEMPOMAP: a time signature, from where it starts (mapBeat), as many beats in a bar (numerator, int 1..32) of a
// note value (denominator: 2, 4, 8 or 16): its bars are numerator x 4 / denominator quarter notes long. The first starts at 0.
// (TIMESIGNATURE, with the elements above.)
inline const Property<int> numerator { "numerator", on (TIMESIGNATURE), whole (1.0, 32.0).orElse (4.0), Kind::song };
inline const Property<int> denominator { "denominator", on (TIMESIGNATURE), whole (2.0, 16.0).orElse (4.0).ofPowersOfTwo(), Kind::song };

// On TEMPO and TIMESIGNATURE. double, 0 or more: where it starts, in beats from the song's start.
inline const Property<double> mapBeat { "beat", on (TEMPO, TIMESIGNATURE), within (0.0, maxBeats).orElse (0.0), Kind::song };

//==============================================================================
// A grid's settings (GridSettings): the playlist's on PHAZITE, the piano roll's on PIANOROLL. view

// bool, default true: positions snap to the grid.
inline const Property<bool> gridSnap { "gridSnap", on (PHAZITE, PIANOROLL), byDefault (true), Kind::view, uncountedOn (PHAZITE) };

// bool, default true: the grid follows the zoom.
inline const Property<bool> gridAdaptive { "gridAdaptive", on (PHAZITE, PIANOROLL), byDefault (true), Kind::view, uncountedOn (PHAZITE) };

// int -3..3, default 0: how many levels coarser (above 0) or finer than the zoom suggests an adaptive grid is.
inline const Property<int> gridOffset { "gridOffset", on (PHAZITE, PIANOROLL), whole (-3.0, 3.0), byDefault (0),
                                        Kind::view,   uncountedOn (PHAZITE) };

// The size of a fixed grid, used while gridAdaptive is off: "1/16 beat", "1/8 beat", "1/4 beat" (the default), "1/2 beat",
// "1 beat", "1 bar", "2 bars", "4 bars" or "8 bars".
inline const Property<juce::String> gridStep { "gridStep", on (PHAZITE, PIANOROLL), byDefault ("1/4 beat"), Kind::view,
                                               uncountedOn (PHAZITE) };

// bool, default false: the grid divides beats into triplets.
inline const Property<bool> gridTriplets { "gridTriplets", on (PHAZITE, PIANOROLL), byDefault (false), Kind::view, uncountedOn (PHAZITE) };

// Child of PHAZITE: the piano roll's grid settings, added when they are first written. view
// (PIANOROLL, with the elements above.)

//==============================================================================
// The song's tracks (model/arrangement/internal/Tracks.h): the playlist's tracks and folders and the mixer's returns,
// top to bottom, each folder's tracks in it, with what each is, its name and color, its mute and solo, its key, its
// audio input and how it shows. song, but for what says it is view

// Child of PHAZITE, which every project has (Project adds it as the project opens): the song's tracks (TRACK), top to
// bottom.
// (TRACKS, with the elements above.)

// Child of TRACKS, or of a folder's TRACK: a track.
// (TRACK, with the elements above.)

// On TRACK, and on MASTER. int64, more than 0: its ID (TrackID), which the song gives it (Song::makeID()).
inline const Property<std::int64_t> trackID { "id", on (TRACK, MASTER), Form::ownID, Kind::song };

// On TRACK: what it is: trackIsTrack (a track, which holds clips), trackIsFolder (a folder, which holds tracks) or
// trackIsReturn (a return, which takes in what tracks send to it, and which no solo silences).
inline constexpr const char* trackIsTrack = "track";
inline constexpr const char* trackIsFolder = "folder";
inline constexpr const char* trackIsReturn = "return";
inline const Property<juce::String> trackKind { "kind", on (TRACK), oneOf (trackIsTrack, trackIsFolder, trackIsReturn), always,
                                                Kind::song };

// On TRACK: its name; empty or none for a track's number, "Track 3", as it is counted among the tracks and returns.
inline const Property<juce::String> trackName { "name", on (TRACK), Kind::song };

// On TRACK: its color, as juce::Colour::toString() writes it; none for its row's in the palette (TrackColours.h).
inline const Property<juce::String> trackColour { "colour", on (TRACK), Form::colour, Kind::song };

// On TRACK. bool, default false: it is muted; and it is soloed. Neither is an undo step (as in many DAWs). song
inline const Property<bool> trackMute { "mute", on (TRACK), byDefault (false), Kind::song };
inline const Property<bool> trackSolo { "solo", on (TRACK), byDefault (false), Kind::song };

// On TRACK, and on MASTER: how the track writes its automation as the song plays (AutomationMode): automationTouch,
// automationLatch or automationWrite; none = read, which plays its lanes and writes nothing. Changing it is no undo step,
// as mute and solo aren't. song
inline constexpr const char* automationTouch = "touch";
inline constexpr const char* automationLatch = "latch";
inline constexpr const char* automationWrite = "write";
inline const Property<juce::String> trackAutomationMode { "automationMode", on (TRACK, MASTER),
                                                          oneOf (automationTouch, automationLatch, automationWrite), Kind::song };

// On a folder's TRACK. true, or none: the folder was made a bus with the bus switch, so it stays one without effects.
inline const Property<bool> folderBus { "bus", on (TRACK), Kind::song };

// On TRACK, and on MASTER: the channel's volume, in decibels (-100 for silence), as its fader sets it (a
// track's, a return's or a bus's; a plain folder's VCA), none for 0 dB, or for the master's -3 dB, a new song's level; and
// its pan, from -1 (left) to 1 (right), none for the centre, which a plain folder keeps for when it is a bus again.
inline const Property<float> channelVolume { "volume", on (TRACK, MASTER, CHAIN), within (-100.0, 6.0), Kind::song };
inline const Property<float> channelPan { "pan", on (TRACK, MASTER, CHAIN), within (-1.0, 1.0), byDefault (0.0f), Kind::song };

// On TRACK, and on MASTER: the device that sets the channel's volume, by its ID (DeviceID, of the song's), which the
// automation and the modulation of its volume and pan name. A track's, a return's, a bus's (a folder that mixes its
// tracks) and the master's is a fader (channelFader: its volume and pan, with its meter after it); a plain folder's is a
// VCA (channelVCA), which sets its tracks' volume. A folder that becomes a bus or plain again gets one of the other kind,
// with an ID of its own, which its volume's automation and modulation then name; its pan's, which a VCA hasn't, go.
inline const Property<std::int64_t> channelFader { "fader", on (TRACK, MASTER), Form::songID, Kind::song };
inline const Property<std::int64_t> channelVCA { "vca", on (TRACK, MASTER), Form::songID, Kind::song };

// The parameters of a channel's fader and of a send, by the IDs their curves and lanes name them with (curveParameter,
// laneParameter): a track's, a return's or a bus's volume and pan, the master's, a plain folder's VCA's volume, and a send's
// level. A volume and a send's level are a fader's position from 0 to 1 (engine/nodes/Channel.h: faderPositionToDecibels()),
// a pan from -1 (left) to 1 (right). song
inline constexpr const char* faderVolume = "volume";
inline constexpr const char* faderPan = "pan";
inline constexpr const char* masterVolume = "master volume";
inline constexpr const char* masterPan = "master pan";
inline constexpr const char* vcaVolume = "vca";
inline constexpr const char* sendLevelParameter = "send level";

// On a track's TRACK. true, or none: the track plays its frozen file (model/mixer/internal/Freeze.h), which is
// in the project's Cache folder, in place of its clips and the devices before its freeze point. Neither freezing nor
// unfreezing is an undo step; a track whose file is gone as the project opens plays live.
inline const Property<bool> trackFrozen { "frozen", on (TRACK), Kind::song };

// A key, on a TRACK (song), on LASTKEY (state) and on CHORDKEY (song): the root, int 0..11 (0 is C), none = no key; and the
// scale: "major", "minor", "dorian", "phrygian", "lydian", "mixolydian", "locrian", "harmonicMinor" or "melodicMinor".
inline const Property<int> keyRoot { "keyRoot", on (TRACK, LASTKEY, CHORDKEY), whole (0.0, 11.0), Kind::song };
inline const Property<juce::String> keyScale { "keyScale", on (TRACK, LASTKEY, CHORDKEY), Kind::song };

// On TRACK (model/transport/RecordingModel.h; roadmap step 3.4). The audio device's input the track records (AudioInput::id):
// a channel, counted from 1 ("1"), or a pair of neighbours from an odd-numbered one ("1+2"); none for no input.
inline const Property<juce::String> trackInput { "input", on (TRACK), Kind::song };

// On a TRACK that plays a drum sampler's pad on a mixer channel of its own (roadmap step 4.12;
// model/devices/internal/DrumKit.h), as FL Studio's FPC sends a pad to a mixer track: the sampler, by its device's ID
// (DeviceID, int64), and the pad, by its note (int, 0 to 127). The pad's sound leaves the sampler's track for it. None for
// any other track.
inline const Property<std::int64_t> trackPadDevice { "padDevice", on (TRACK), Form::idName, Kind::song };
inline const Property<int> trackPad { "pad", on (TRACK), whole (0.0, 127.0), Kind::song };

// On TRACK. When the track's input is heard through it: monitorAlways, monitorNever, or none, while it is armed.
inline constexpr const char* monitorAlways = "always";
inline constexpr const char* monitorNever = "never";
inline const Property<juce::String> trackMonitoring { "monitoring", on (TRACK), oneOf (monitorAlways, monitorNever), Kind::song };

// The view state below is the master's too (its row), and none of it a change (isUncountedProperty()).

// On TRACK. int 32..320, default 64: the track's height in the playlist, in pixels at an interface scale of 100%. view
inline const Property<int> trackHeight { "height", on (TRACK, MASTER), whole (32.0, 320.0), byDefault (64), Kind::view, uncounted };

// On TRACK. bool, default false: the track is a thin row with only its name; a collapsed folder hides its tracks. view
inline const Property<bool> trackCollapsed { "collapsed", on (TRACK, MASTER), byDefault (false), Kind::view, uncounted };

// On TRACK. bool, default false: the track stays in view while the others scroll. view
inline const Property<bool> trackPinned { "pinned", on (TRACK, MASTER), byDefault (false), Kind::view, uncounted };

// On TRACK. bool: the piano roll shows the track's clips in drum mode; none = on while the track's instrument is
// Phazite's drum sampler. view
inline const Property<bool> drumMode { "drumMode", on (TRACK, MASTER), Kind::view, uncounted };

// On TRACK: the order of the drum rows, as note numbers separated by commas, top first; rows it doesn't list come
// after them. view
inline const Property<juce::String> drumOrder { "drumOrder", on (TRACK, MASTER), Kind::view, uncounted };

// On TRACK: the drum rows hidden, as note numbers separated by commas. view
inline const Property<juce::String> drumHidden { "drumHidden", on (TRACK, MASTER), Kind::view, uncounted };

// Child of PHAZITE, which every project has: the master channel's own element, with its write mode
// (trackAutomationMode), its volume and pan (channelVolume, channelPan), its devices, and its automation lanes (LANES,
// view), with an ID of the song's (trackID) like a track's. song, but for its lanes
// (MASTER, with the elements above.)

// Child of TRACK and of MASTER, once the track has kept a lane: its automation lanes (LANE), in an element of their own.
// Lanes are view state, added and removed outside the undo history, which finds a track's other elements (its clips,
// sends, devices and modulators) by their places among the track's elements: a lane added or removed among them would
// make an undo or a redo take the wrong one. So LANES is added after the track's elements, once, and stays while the song
// is open, empty or not. view
// (LANES, with the elements above.)

// Child of LANES, one for each automation lane the playlist shows under the track, in their order (docs/design.md,
// section 10): the automation of a parameter of one of the track's devices, its volume and pan included. A lane of a
// device that isn't on the track any more is ignored, and a parameter with automation but no lane here shows one after
// these. view
// (LANE, with the elements above.)

// On LANE: the device, by its ID written as text (DeviceID: "1042").
inline const Property<juce::String> laneDevice { "device", on (LANE), Form::idName, Kind::view };

// On LANE: the parameter, by its ID in its device, as its curve names it (curveParameter).
inline const Property<juce::String> laneParameter { "parameter", on (LANE), Kind::view };

// On LANE. int 32..320, default 48: the lane's height in the playlist while it isn't collapsed, in pixels at an interface
// scale of 100%.
inline const Property<int> laneHeight { "height", on (LANE), whole (32.0, 320.0), byDefault (48), Kind::view };

// On LANE. true, or none: the lane shows as a thin row.
inline const Property<bool> laneCollapsed { "collapsed", on (LANE), Kind::view };

//==============================================================================
// The song's devices (model/mixer/internal/Mixer.h): each track's, a folder's (as a bus) and the master's, in the order its
// sound goes through them, an instrument first, each with what it is set to, and each container with its devices. song

// Child of TRACK and of MASTER: a device. On DEVICE: its ID (DeviceID, of the song's: Song::makeID()); its type:
// the typeID of one of Phazite's own (src/devices/<area>/: "phaziteEq"), deviceIsPlugin for a plug-in, or the type of a
// device registered for the tests (model/devices/internal/Devices.h: registerDeviceType()); and deviceOff, bool, default
// false: it is switched off.
// (DEVICE, with the elements above.)
inline const Property<std::int64_t> deviceID { "id", on (DEVICE, CONTAINER), Form::ownID, Kind::song };
inline const Property<juce::String> deviceType { "type", on (DEVICE), Kind::song };
inline constexpr const char* deviceIsPlugin = "plugin";
inline const Property<bool> deviceOff { "off", on (DEVICE, CONTAINER, MODULATOR), byDefault (false), Kind::song };

// The format a device type of Phazite's own names in its description (model/mixer/DeviceType.h), as a plug-in's names
// "VST3", with its type ID as the description's identifier: what the browser, the menus and a drag carry; never saved.
inline constexpr const char* ownDeviceFormat = "Phazite";

// On a DEVICE: the track it listens to through its sidechain, by the track's ID (TrackID); none for none.
inline const Property<std::int64_t> deviceSidechain { "sidechain", on (DEVICE), Form::idName, Kind::song };

// On a DEVICE: where its window was, the left and the top of it on the screen (int, pixels), where it opens
// again; none for a window never moved. view
inline const Property<int> deviceWindowX { "windowX", on (DEVICE), Kind::view, uncounted };
inline const Property<int> deviceWindowY { "windowY", on (DEVICE), Kind::view, uncounted };

// Children of one of Phazite's own devices' DEVICE: VALUES, its parameters' values as they were set by hand (not as
// automation or a modulator moves them), each a property named by the parameter's ID in the device's description, none
// for one that was never set (at its default); and SOUNDS, the factory sounds, files, tables and slices it plays, as
// properties named by getPadSound(), getPadSoundFile(), getOscillatorWavetable(), getOscillatorWavetableFile(),
// samplerSample, samplerSampleFile, samplerSlices and samplerSlicesRemoved (below). Both as the song holds them from the
// moment it opens (DevicesValue.h: keepDeviceAsHeld(), the loader's): no value at its parameter's default, no sound of a
// property the device's type hasn't nor an empty one, no VALUES or SOUNDS left empty, nothing Phazite doesn't know in either;
// a plug-in has neither. The devices' values (DevicesValue.h) hold them by their indices; a DEVICE's, a CONTAINER's and a
// CHAIN's properties below are the values' too, but a plug-in's state, program, format ID and version, its VALUE children, a
// chain's mute and solo and a window's place.
// (VALUES and SOUNDS, with the elements above.)

// On a plug-in's DEVICE: which plug-in it is, as scanning found it and a preset names it (preset::plugin):
// its format ("VST3"), name, maker (manufacturer), identifier (its file on the computer it was added on, or what its
// format finds it by) and unique ID (int: JUCE's number for it, which for a VST3 is made from its class ID). A song finds
// its plug-in by its format, unique ID and maker first, wherever this computer keeps its file
// (model/mixer/internal/Mixer.h: findKnownPlugin()), then by its file and name, then by its name.
inline const Property<juce::String> pluginFormat { "format", on (DEVICE), Kind::song };
inline const Property<juce::String> pluginName { "name", on (DEVICE), Kind::song };
inline const Property<juce::String> pluginMaker { "manufacturer", on (DEVICE), Kind::song };
inline const Property<juce::String> pluginIdentifier { "identifier", on (DEVICE), Kind::song };
inline const Property<int> pluginUid { "uid", on (DEVICE), Kind::song };
// On a plug-in's DEVICE: true for an instrument, which turns notes into sound, as its description says; none for an effect.
inline const Property<bool> pluginInstrument { "instrument", on (DEVICE), byDefault (false), Kind::song };

// On a plug-in's DEVICE and on a CONTAINER: its mix, 0 to 1, as setMix() sets it; none for 1.
inline const Property<float> deviceMix { "mix", on (DEVICE, CONTAINER), within (0.0, 1.0), byDefault (1.0f), Kind::song };

// On a plug-in's DEVICE: what the plug-in keeps of its own, which the project stores outside the undo history as it saves,
// before it copies the plug-in or takes it away, and as a preset loads into it (model/plugins/internal/PluginInstances.h):
// its state, the bytes it keeps its settings in (in base 64, as RFC 4648 has it: model/base/internal/SongText.h), none for
// none, which for a VST3 are its format's own preset file, a .vstpreset (sandbox/core/Vst3State.h), as DAWproject holds a
// VST3's state too; and its program (int), for a plug-in with programs. state
inline const Property<juce::MemoryBlock> pluginState { "state", on (DEVICE), Kind::state };
inline const Property<int> pluginProgram { "program", on (DEVICE), Kind::state };

// On a plug-in's DEVICE, stored with its state: its ID in its format, which other hosts know it by (a VST3's class ID, 32
// hexadecimal digits, as its preset file holds it), none for another format; and the version of the plug-in that made the
// state, as the plug-in tells it ("1.3.7"), none if it tells none. state
inline const Property<juce::String> pluginFormatID { "formatID", on (DEVICE), Kind::state };
inline const Property<juce::String> pluginVersion { "version", on (DEVICE), Kind::state };

// Children of a plug-in's DEVICE: the values set by hand of its parameters, those set by Phazite's controls, and those of
// the parameters that automation or a modulator moves away from them, stored with its state (its state holds the others),
// each a VALUE with the parameter's ID, the plug-in's own (a VST3's number, which can't name a property in a file:
// sandbox/core/PluginInfo.h), and its value set by hand (float, 0 to 1).
// (VALUE, with the elements above.)
inline const Property<juce::String> valueParameter { "parameter", on (VALUE), Kind::song };
inline const Property<float> valueSetByHand { "value", on (VALUE), within (0.0, 1.0).orElse (0.0), Kind::song };

// Child of TRACK and of MASTER, among its devices: a container (model/mixer/internal/Containers.h), with its ID, deviceOff
// and deviceMix, as a device has them, and its name. Its children are its chains (CHAIN), one at least, in their order, each
// with its devices, and its macros (MACRO), with what they modulate. A container of effects plays its chains side by side,
// each on its part of the sound that comes in (containerSplit), and adds up what they put out; an instrument container
// (containerInstrument), which is a track's instrument, plays its chains' instruments, each on the notes in its ranges.
// (CONTAINER, with the elements above.)
inline const Property<juce::String> containerName { "name", on (CONTAINER), Kind::song };

// On a CONTAINER of effects: what part of the sound each chain plays: splitBands, a band of it, low to high in the chains'
// order, each from its chain's crossover (the first from the bottom); splitMidSide, the first its middle (what its sides
// share) and the second its side (what they don't); splitLeftRight, the first its left and the second its right; none for
// all of it, each chain a layer. The parts add up to the sound, so chains with nothing in them pass it as it came, but
// layers, which add up as many times.
inline constexpr const char* splitBands = "bands";
inline constexpr const char* splitMidSide = "midSide";
inline constexpr const char* splitLeftRight = "leftRight";
inline const Property<juce::String> containerSplit { "split", on (CONTAINER), oneOf (splitBands, splitMidSide, splitLeftRight),
                                                     Kind::song };

// On a CONTAINER. true, or none: it is an instrument container, a track's instrument, each of whose chains holds an
// instrument first and the effects after it, and plays the notes in its ranges.
inline const Property<bool> containerInstrument { "instrument", on (CONTAINER), byDefault (false), Kind::song };

// Child of CONTAINER: a chain, in their order, its devices (DEVICE) its children in the order its sound goes through them. On
// CHAIN: its ID (DeviceID, of the song's), which its parameters' curves, lanes and modulations name; its name; its volume
// and pan (channelVolume, channelPan), as a channel's; chainMute and chainSolo, true or none, set as a track's mute and solo
// are, without undo steps; on a container split into bands, its chainCrossover, in hertz (float): where its band starts,
// the first chain's from the bottom, none for its place's default (model/mixer/internal/Containers.h:
// getDefaultCrossover()); and on an instrument container's, the notes it plays: chainLowKey and chainHighKey (int 0..127,
// none for 0 and 127) and chainLowVelocity and chainHighVelocity (int 1..127, none for 1 and 127).
// (CHAIN, with the elements above.)
inline const Property<std::int64_t> chainID { "id", on (CHAIN), Form::ownID, Kind::song };
inline const Property<juce::String> chainName { "name", on (CHAIN), Kind::song };
inline const Property<bool> chainMute { "mute", on (CHAIN), Kind::song };
inline const Property<bool> chainSolo { "solo", on (CHAIN), Kind::song };
inline const Property<float> chainCrossover { "crossover", on (CHAIN), within (20.0, 20000.0), Kind::song };
inline const Property<int> chainLowKey { "lowKey", on (CHAIN), whole (0.0, 127.0), byDefault (0), Kind::song };
inline const Property<int> chainHighKey { "highKey", on (CHAIN), whole (0.0, 127.0), byDefault (127), Kind::song };
inline const Property<int> chainLowVelocity { "lowVelocity", on (CHAIN), whole (1.0, 127.0), byDefault (1), Kind::song };
inline const Property<int> chainHighVelocity { "highVelocity", on (CHAIN), whole (1.0, 127.0), byDefault (127), Kind::song };

// The parameters of a container's chain, by the IDs their curves, lanes and modulations name them with, the chain's ID
// being their device's: its volume, as a fader's position from 0 to 1; its pan, from -1 to 1; and its crossover, in hertz.
// song
inline constexpr const char* chainVolumeParameter = "volume";
inline constexpr const char* chainPanParameter = "pan";
inline constexpr const char* crossoverParameter = "crossover";

// Child of a track's TRACK: a send to a return, in the order the track's sends were added (model/mixer/internal/Mixer.h).
// On SEND: its ID (DeviceID, of the song's); the return it sends to, by the return's track's ID (sendTo, written as a number,
// as every ID an item holds is, unlike a device a lane names); its level in decibels, as a fader's (sendLevel; -100 for
// silence; default 0 dB, written as none); and sendPreFader, true for a send before the track's fader (default false, a send
// after it, written as none). A send goes with its return, in the same undo step; one to a return the song hasn't goes as
// the project opens. What reads a send reads it as a value (model/base/internal/SendsValue.h: SendValue, and the conversion
// of SEND); until the sends leave the tree (roadmap step S5.20), their writers (model/mixer/Mixer.cpp) and the loader, the
// tree's callbacks that hear a send change (until the sends' second piece), the compile's modulations (until modulation's
// pieces), the versions' difference and merge on whole copies, and the selection's walk, which skips a SEND, still name it
// (SendsValue.h, at its top, lists them).
// (SEND, with the elements above.)
inline const Property<std::int64_t> sendID { "id", on (SEND), Form::ownID, Kind::song };
inline const Property<std::int64_t> sendTo { "to", on (SEND), Form::idName, Kind::song };
inline const Property<float> sendLevel { "level", on (SEND), within (-100.0, 6.0), byDefault (0.0f), Kind::song };
inline const Property<bool> sendPreFader { "preFader", on (SEND), byDefault (false), Kind::song };

//==============================================================================
// The song's modulation (model/modulation/internal/Modulation.h): each track's modulators and macros, and a container's
// macros, with what each modulates. song

// Child of TRACK and of MASTER: a modulator, in their order. On MODULATOR: its ID (ModulatorID, of the song's); its type,
// one of the words below, in ModulatorType's order (model/modulation/ModulationValues.h; preset::modulatorTypeNames lists
// them so): an LFO, an envelope follower, a step sequencer, a random source, a breakpoint envelope or a note follower
// ("lfo"); and deviceOff, true when it is switched off. Its parameters' values set by hand are its VALUES, by their IDs in
// its description (devices/modulation/Modulators.h: "rate"), as a device's are, none for one never set; a step sequencer's
// steps are modulatorSteps: each step's value, -1 to 1, to a ten-thousandth, separated by spaces, without the zeros at the
// end. The model reads and writes them through their values (model/base/internal/ModulationValue.h). A song holds each
// modulator and macro as its value holds it, its element written as Phazite writes that value (its VALUES before its
// MODULATES), from the moment it opens (the loader's keepModulatorAsHeld()): so the song keeps no property or element
// Phazite doesn't know on a MODULATOR, a MACRO, their MODULATES or a MODULATOR's VALUES, nor a value at its default, nor a
// choice written as its number's text.
// (MODULATOR, with the elements above.)
inline constexpr const char* modulatorIsLfo = "lfo";
inline constexpr const char* modulatorIsEnvelopeFollower = "envelopeFollower";
inline constexpr const char* modulatorIsSteps = "steps";
inline constexpr const char* modulatorIsRandom = "random";
inline constexpr const char* modulatorIsBreakpoints = "breakpoints";
inline constexpr const char* modulatorIsNoteFollower = "noteFollower";
inline const Property<std::int64_t> modulatorID { "id", on (MODULATOR, MACRO), Form::ownID, Kind::song };
inline const Property<juce::String> modulatorType { "type", on (MODULATOR),
                                                    oneOf (modulatorIsLfo, modulatorIsEnvelopeFollower, modulatorIsSteps, modulatorIsRandom,
                                                           modulatorIsBreakpoints, modulatorIsNoteFollower),
                                                    Kind::song };
inline const Property<juce::String> modulatorSteps { "steps", on (MODULATOR), Form::numbers, within (-1.0, 1.0).orElse (0.0), Kind::song };

// Child of a track's TRACK and of a CONTAINER: a macro, in their order (a track's on tracks and returns, not on folders;
// a container has eight). On MACRO: its ID (ModulatorID, of the song's); its name; and its value, 0 to 1, default 0, none
// for 0.
// (MACRO, with the elements above.)
inline const Property<juce::String> macroName { "name", on (MACRO), Kind::song };
inline const Property<float> macroValue { "value", on (MACRO), within (0.0, 1.0), byDefault (0.0f), Kind::song };

// Child of MODULATOR and of MACRO: what it modulates, in the order its modulations were made. On MODULATES: the parameter,
// by its device's ID as a curve names it (curveDevice: "1042") and its own ID; and the amount, -1 to 1, to a
// ten-thousandth.
// (MODULATES, with the elements above.)
inline const Property<juce::String> modulatesDevice { "device", on (MODULATES), Form::idName, Kind::song };
inline const Property<juce::String> modulatesParameter { "parameter", on (MODULATES), Kind::song };
inline const Property<float> modulatesAmount { "amount", on (MODULATES), within (-1.0, 1.0).orElse (0.0), Kind::song };

//==============================================================================
// The song's automation (model/automation/internal/Automation.h): each automated parameter's curve, by its device and its
// own ID, with its points in beats. song

// Child of PHAZITE: the curves (CURVE), in the order the song made them (a curve made goes after the others). Written
// while the song has a curve. A curve whose device the song hasn't any more stays while an undo could bring the device back,
// and goes as the project opens. The song holds its curves as values beside its tree (model/base/internal/SongValues.h:
// the curves' area), its AUTOMATION an empty place every song that holds its values has; what Phazite doesn't know on it, a
// curve or a point (a property, a child of another type) goes as the song opens.
// (AUTOMATION, with the elements above.)

// Child of AUTOMATION: a parameter's curve, with its points (POINT) in time order, two at one beat in the order they go (a
// jump), and nothing else: a child of another type goes as the song loads (model/project/Project.cpp), so that its points
// are its children; one curve of a parameter, the first. On CURVE: the device, by its ID written as text (DeviceID:
// "1042"); and the parameter, by its ID in its device, as a lane names them (laneDevice, laneParameter).
// (CURVE, with the elements above.)
inline const Property<juce::String> curveDevice { "device", on (CURVE), Form::idName, Kind::song };
inline const Property<juce::String> curveParameter { "parameter", on (CURVE), Kind::song };

// Child of CURVE: a point. On POINT: where it is, in beats from the song's start (double, 0 or more); where the parameter's
// control is there, as a position along its travel (float 0..1, from all the way down or left to all the way up or right,
// as its knob or fader moves, with the parameter's skew: model/automation/AutomationValues.h), so that a straight line is an
// even turn of the knob; and how the line to the next point bends (float -1..1, default 0: straight, written only while it
// isn't). A point whose value is no number goes as the song loads (model/project/Project.cpp), with a curve left without
// points.
// (POINT, with the elements above.)
inline const Property<double> pointBeat { "beat", on (POINT), within (0.0, maxBeats).orElse (0.0), Kind::song };
inline const Property<float> pointValue { "value", on (POINT), within (0.0, 1.0), Kind::song };
inline const Property<float> pointCurve { "curve", on (POINT), within (-1.0, 1.0), byDefault (0.0f), Kind::song };

//==============================================================================
// The song's MIDI links (model/midi/internal/MidiLinks.h): a hardware controller linked to the parameter it turns (MIDI
// learn). song

// Child of PHAZITE, added with the first link: the links (MIDILINK), in the song's order (of two of one controller or one
// parameter, the first stays). A link of a device, a chain, a send, a fader, a modulator or a macro that goes leaves with it,
// in its step (model/mixer/internal/Mixer.h: forgetWhatNamesTheGone()). A song's links are a value beside its tree while it is
// open (model/base/internal/MidiLinksValue.h), each property below as its description reads it, and its tree's MIDILINKS
// an empty placeholder, kept where the song has one: so the song keeps no property or element Phazite doesn't know on a
// MIDILINKS or a MIDILINK.
// (MIDILINKS, with the elements above.)

// Child of MIDILINKS: a link. On MIDILINK: the controller, by its channel (int 1..16) and its number (int 0..119: 120 to
// 127 are channel mode messages, which no link takes; a link with another goes as the song loads); and the parameter it
// turns, by its device's ID as a curve names it (curveDevice: "1042", a macro's or a modulator's own ID for theirs) and its
// own ID in it (curveParameter: a modulator's parameter by its name, "rate"; a macro's value by the word "value",
// midiLinkMacroValue, so that a macro given a new ID by a merge or a copy keeps its link). One link of a controller, and
// one of a parameter.
// (MIDILINK, with the elements above.)
inline const Property<int> midiLinkChannel { "channel", on (MIDILINK), whole (1.0, 16.0).goingOutside(), Kind::song };
inline const Property<int> midiLinkNumber { "number", on (MIDILINK), whole (0.0, 119.0).goingOutside(), Kind::song };
inline const Property<juce::String> midiLinkDevice { "device", on (MIDILINK), Form::idName, Kind::song };
inline const Property<juce::String> midiLinkParameter { "parameter", on (MIDILINK), Kind::song };
inline constexpr const char* midiLinkMacroValue = "value";

// View settings: saved, written without undo steps, and not unsaved changes (decision 4; isUncountedProperty() and
// isUncountedElement(), below): on tracks and the master (whose row Collapse All collapses), their heights, collapsed and
// pinned, drum mode and rows, and their automation lanes; on PHAZITE, the playlist's grid; on devices (DEVICE), where their
// windows were. PIANOROLL and LANES hold view settings only. These lists are made from the descriptions (their kind view,
// and the elements they are on): anything else a project holds is song or state.
using IdentifierList = std::vector<const juce::Identifier*>;

inline bool isAmong (const juce::Identifier& p, const IdentifierList& list)
{
    return std::any_of (list.begin(), list.end(), [&p] (const juce::Identifier* name) { return *name == p; });
}

// The properties described on an element type of a kind, in their order.
IdentifierList findProperties (const juce::Identifier& type, Kind);

inline const IdentifierList& getTrackViewProperties()
{
    static const IdentifierList list = findProperties (TRACK, Kind::view);
    return list;
}

inline const IdentifierList& getGridProperties()
{
    static const IdentifierList list = findProperties (PIANOROLL, Kind::view);
    return list;
}

// Written without the undo manager (model/mixer/internal/Mixer.h: rememberWindowPosition()).
inline const IdentifierList& getDeviceViewProperties()
{
    static const IdentifierList list = findProperties (DEVICE, Kind::view);
    return list;
}

inline bool isTrackViewProperty (const juce::Identifier& p)
{
    return isAmong (p, getTrackViewProperties());
}
inline bool isGridProperty (const juce::Identifier& p)
{
    return isAmong (p, getGridProperties());
}
inline bool isDeviceViewProperty (const juce::Identifier& p)
{
    return isAmong (p, getDeviceViewProperties());
}

// An element of a track's that is view state, with everything in it: one that isn't counted, in a track (LANES).
inline bool isTrackViewElement (const juce::ValueTree& element)
{
    const auto* described = getTables().findElement (element.getType());
    return described != nullptr && ! described->isCounted() && described->goesIn (TRACK);
}

// What a project saves that is no change to it: view state (above); how the song plays rather than the song (design question
// 18): the loop (where it is, whether it is on, and whether it follows the song), the metronome's switch, where playback
// starts, the punch and the input chosen last; the song's next ID (nextID), which the item it numbers comes with; and where
// the song stands in the project's versions (versionsBranch, versionsBase), which the versions keep as they move. As a
// property (isUncountedProperty()), or as an element with everything in it (isUncountedElement()). A project's unsaved
// changes (model/project/internal/Project.h) and the versions' differences (model/versions/) leave out the same.
bool isUncountedProperty (const juce::ValueTree&, const juce::Identifier& property);
bool isUncountedElement (const juce::ValueTree& parent, const juce::ValueTree& element);

// Whether a property of an element, or the element itself with everything in it, counts: not uncounted, nor in an
// uncounted element.
bool isCounted (const juce::ValueTree&, const juce::Identifier& property);

//==============================================================================
// How the versions (model/versions/) compare two songs, merge them, and make one like the other.

// What tells an element of one song for the same element of another, among its parent's children: its type and its key.
// An item by its ID (each kind's is named "id": TRACK, MASTER, DEVICE, CONTAINER, CHAIN, SEND, MODULATOR, MACRO, CLIP,
// NOTE, SECTION); a curve, a lane and a modulation by their device and parameter; a plug-in's VALUE by its parameter; a tempo,
// a time signature, a chord and a key by where they start; and an element there is one of in its parent (TEMPOMAP, TRACKS,
// VALUES, SOUNDS...) by its type alone. Empty for what has no key of its own: a curve's points and a clip's controller
// events, which their parent's elements of that kind hold as a whole.
juce::String getElementKey (const juce::ValueTree&);

// Whether the order of an element among its parent's elements means something, and which order it is in: the same name
// for the elements of one order ("chain" for a track's devices and containers, or a container's chain's devices, in the
// order its sound goes through them; "chains" for a container's chains), and empty for those in no particular order (clips,
// notes, curves, sections, chords, tempos).
juce::String getOrderGroup (const juce::ValueTree&);

// The properties that hold an ID of the song's: an item's own (each kind's "id", a note's too), a channel's fader and VCA,
// and an alias group (clipLink). And those that name one: a send's return (sendTo), a device's sidechain, and a lane's, a
// curve's and a modulation's device. What a copy gives anew and renames to match (giveNewIDs()), and so does a merge for
// what the other branch added.
bool holdsID (const juce::ValueTree&, const juce::Identifier& property);

// Where each element of a type Phazite knows goes, by its parent's type ("Child of" above), PHAZITE the song's own; and
// whether Phazite knows a type: one it doesn't (a later build's, another tool's) is kept as it is, with everything in it
// (model/project/internal/Project.h: makeConsistent()), and nothing reads it.
const std::map<juce::String, juce::StringArray>& getPlaces();
bool isKnownType (const juce::Identifier&);

// What every ID of the song's is below: one at or past it (a file from others can hold any number) is none the song can
// count on from, and is given anew as the song loads (model/project/internal/Project.h: makeConsistent()).
inline constexpr std::uint64_t maxID = (std::uint64_t) 1 << 62;

// Room to count on. A song whose largest ID (findLargestID()) or next ID reaches renumberMark, 3 * 2^60, 2^60 below
// maxID, is given anew, once, as it loads, every ID at or past maxCountedID, 2^61, and below maxID (mustGiveIDsAnew()):
// counted on from past every ID it holds below maxCountedID and every ID a name names below it; what named each naming
// its new one, and a name at or past maxCountedID that names none of them left out; an alias group's notes given the same
// new IDs in each of its clips; a track given a new ID no longer frozen; every merge waiting dropped; and the song stamped
// with its new numbering (idNumbering). Any other song keeps
// its IDs and its next ID as they are: as it loads, its next ID is below the mark (at most maxCountedID plus the IDs
// given anew, where they were), and a session makes far fewer than the 2^60 IDs between.
// The one way across, accepted in the known-bugs round (2026-11-16): a file whose largest ID or next ID is within one
// session's count below the mark (another program's, or Phazite's own after a session on such a file) opens as it is,
// what the session makes gets IDs at or past the mark, and the next opening gives every ID at or past maxCountedID anew,
// once (tests/InconsistentProjectTests.cpp pins it). The song keeps all it holds: names follow, an alias group's clips
// keep sharing their notes' IDs, and it holds together. What goes: a track's frozen state (freeze it again); a merge
// waiting. Refused with its reason: Merge and Restore of the commits and Local History snapshots made after the crossing,
// whose songs are given anew as they are read; and Restore of a line and Merge against those made before it (on that
// file, before and in that session), which hold the IDs as they were and not the song's stamp (idNumbering): matched by
// ID, a track restored from one would come back beside the track it was given anew as, and a Merge whose parting is one
// of them would bring its tracks in again (known bug 186). What makes the song another whole (a version opened, A/B, a
// branch switch, Restore This Version, Restore from Local History) takes such a song with its numbering, as a switch
// between two branches that each counted on from one version takes songs whose IDs name different things. Such a whole
// song from before put in the song's place also takes that version's count of IDs, near the mark, which Undo doesn't put
// back (nextID is written without an undo step): a later session can cross the mark again, the next opening gives the
// IDs anew once more with another stamp, and the versions saved between the two crossings are then refused as those from
// before the first are.
// The first commit after lists every item given anew as changed, and the history keeps that commit's changes as such. A
// branch switch to a commit made after the crossing gives its song new IDs as it is read, without the values of its
// plug-ins' parameters, which the commit keeps by device ID (model/versions/SongPieces.cpp: joinSong()). No rule on the
// IDs' values alone can close it: whatever it accepts has an edge a session can step past, and a second mark only moves
// the edge.
inline constexpr std::uint64_t maxCountedID = maxID / 2;
inline constexpr std::uint64_t renumberMark = maxID - ((std::uint64_t) 1 << 60);
bool namesID (const juce::ValueTree&, const juce::Identifier& property);

// How a version's pieces split a song (model/versions/internal/SongPieces.h), which a reader needn't know: the elements its
// piece holds as pieces of their own (a track, the master, a clip, a device, a container, an automation curve), and those
// that go in runs once their parent has many (a clip's notes, a curve's points).
bool isPieceOfItsOwn (const juce::ValueTree&);
bool goesInRuns (const juce::ValueTree&);

// The properties whose values are bytes, written in base 64 (model/base/internal/SongText.h: writeBytes()): a plug-in's
// state (pluginState). A version's pieces keep each out of line, as a piece of its own.
bool isBytesProperty (const juce::ValueTree&, const juce::Identifier& property);

// The properties an element's key is made of (getElementKey()), in the order the key joins them; none for an element
// whose key is its type, or that has none (a curve's points).
juce::StringArray getKeyProperties (const juce::Identifier& type);

// The rules above (getElementKey(), getOrderGroup(), holdsID(), namesID(), isUncountedProperty(), isUncountedElement(),
// isPieceOfItsOwn(), goesInRuns(), isBytesProperty()) as data, for another tool to compare and merge songs as Phazite does:
// what tests/golden/format/song-rules.json holds (written by the golden tests; docs/versions-format.md, section 5).
juce::var describeSongRules();

//==============================================================================
// The project's versions (model/versions/; docs/design/versions.md, and docs/versions-format.md for another tool): the song as it
// was at each version the user saved, and its branches, in the project's Versions folder
// (model/versions/internal/VersionStore.h). Nothing written there changes again, but each computer's record of a branch,
// its own files and the name of the current branch, which are replaced whole, and a branch's log, which is appended to.
//
//   Versions/
//     pieces/<name>.pack                pieces, written together as a version is saved (model/versions/internal/PieceStore.h)
//     branches/<branch>/<computer>.xml  a branch as that computer last had it (BRANCH): only that computer writes it
//     current                           the current branch's id, which goes with the project file
//     local/<computer>/files.xml        what each audio file this computer read hashes to (FILES), so that each is read once
//     local/<computer>/working.xml      the song left on each branch this computer switched from (WORKING)
//     local/<computer>/logs/<branch>    each move of the branch's last version on this computer, a line each
//     bounces/<id>.flac                 a version's bounce, by the version's id: its song rendered as Export Song renders
//                                       it, in FLAC (16 bits, dithered), which a deleted bounce takes with it
//
// A piece is named by the SHA-256 of what it holds, in hexadecimal. It holds an element of the song as XML (UTF-8), as
// model/base/internal/SongText.h writes it: its properties in the order of their names, its children in their order, and
// nothing between them. Its tracks, clips, devices, containers and curves are pieces of their own, each a _piece in its
// place; a clip's many notes and a curve's many points are in runs (_run), each a piece of its own; and a property whose
// value is bytes (a plug-in's state) is a piece of its own, in its bytes (model/versions/internal/SongPieces.h). So a
// version stores only the pieces no version before it has.
namespace versions
{
    // The store's own names in a piece start with "_", which no song's name does (docs/versions-format.md), and a reader
    // leaves out an element of the store's it doesn't know.
    //
    // In its place in its parent's piece, an element that is a piece of its own (_piece): the piece's name (_hash), the
    // element's type (_type), and the properties its key is made of, as the element has them (schema::getKeyProperties()),
    // so that what a piece holds is known without reading it. A run of notes or points is a piece too, of the type "_run",
    // which holds them one after another (_run).
    inline const juce::Identifier PIECE { "_piece" };
    inline const juce::Identifier pieceHash { "_hash" }, pieceType { "_type" };
    inline const juce::Identifier RUN { "_run" };

    // A property whose value is bytes (schema::isBytesProperty(): a plug-in's state), in place of the property: a _bytes
    // element, first among its element's, naming the property (name) and the piece that holds its bytes (hash).
    inline const juce::Identifier BYTES { "_bytes" };
    inline const juce::Identifier bytesName { "name" }, bytesHash { "hash" };

    // On a plug-in's DEVICE in a piece: the piece (hash) of its parameters' values as the plug-in told them when the version
    // was saved (_parameters: a _parameter for each, with its ID, the plug-in's own, its name and its value from 0 to 1),
    // which the changes name the knobs by, as the state's bytes say nothing to anyone but the plug-in. Not part of the song.
    inline const juce::Identifier PARAMETERS { "_parameters" };
    inline const juce::Identifier parametersHash { "hash" };
    inline const juce::Identifier PARAMETER { "_parameter" };
    inline const juce::Identifier parameterID { "id" }, parameterName { "name" }, parameterValue { "value" };

    // On an element joined from a version whose piece, or part of it, couldn't be read: a stand-in that names what is
    // missing by its type and key, which making the song like the version leaves as it is now (model/versions/ApplySong.h).
    // Never part of a song.
    inline const juce::Identifier missing { "_missing" };

    // A version, a piece of its own, named by its piece's name: its song's piece (song); the versions it follows (parents:
    // one, or two for a merge, separated by spaces; none for the first); its name and note, as the user gave them, or none;
    // the summary of its changes, which names it while it has no name of its own; when it was saved (saved, milliseconds
    // since 1970); the branch it was saved on (branch: the branch's id); and the Phazite that saved it (phazite: its
    // version, model/engine/AppInfo.h). With a FILE for each audio file its song plays: its path (as clipFile holds it),
    // its size in bytes, and the SHA-256 of what it holds (sha256). And, for a merge that gave new IDs to what the other line
    // added (model/versions/internal/SongMerge.h), a RENUMBERED for each: the ID as the other line has it (from) and as the
    // merge gave it (to), and for a note its clip's group (clip: "link <alias group>" or "clip <id>"), so that a later merge
    // of that line knows them.
    inline const juce::Identifier VERSION { "VERSION" };
    inline const juce::Identifier versionSong { "song" }, versionParents { "parents" }, versionName { "name" }, versionNote { "note" },
        versionSummary { "summary" }, versionSaved { "saved" }, versionBranch { "branch" }, versionPhazite { "phazite" };
    inline const juce::Identifier FILE { "FILE" };
    inline const juce::Identifier filePath { "path" }, fileSize { "size" }, fileHash { "sha256" };
    inline const juce::Identifier RENUMBERED { "RENUMBERED" };
    inline const juce::Identifier renumberedClip { "clip" }, renumberedFrom { "from" }, renumberedTo { "to" };

    // A branch as one computer last had it, in a file of its own that only that computer writes: its id (a juce::Uuid's 32
    // hexadecimal digits), its name and its color (as juce::Colour::toString() writes it), when they were last set
    // (changed, milliseconds since 1970: the newest computer's win), and its last version there (head; none before its
    // first). The computers' records together make the branch: where one's last version follows every other's, that is the
    // branch's; where not, the lines went their own ways, to be merged (model/versions/internal/VersionHistory.h). Deleted
    // (deleted, 1) on the computer whose record says so: the branch is gone where that record is the newest and its last
    // version follows every other record's, so that versions another computer saved on it since bring it back.
    inline const juce::Identifier BRANCH { "BRANCH" };
    inline const juce::Identifier branchID { "id" }, branchName { "name" }, branchColour { "colour" }, branchHead { "head" },
        branchChanged { "changed" }, branchDeleted { "deleted" };

    // working.xml: WORKING, with a LEFT for each branch this computer left with a song of its own on it: the branch's id
    // (branch) and the song as it was left there (song: its piece, view state included).
    inline const juce::Identifier WORKING { "WORKING" };
    inline const juce::Identifier LEFT { "LEFT" };
    inline const juce::Identifier leftBranch { "branch" }, leftSong { "song" };

    // messages/<computer>.xml, which only that computer writes: MESSAGES, with a MESSAGE for each commit whose message was
    // edited there (Edit Commit Message...): the commit's id (commit), its name and note as they are now, either empty for
    // none, and when they were set (changed, milliseconds since 1970: the newest computer's win). A commit's own record
    // never changes; these say what it is called, as Fossil's amendments do.
    inline const juce::Identifier MESSAGES { "MESSAGES" };
    inline const juce::Identifier MESSAGE { "MESSAGE" };
    inline const juce::Identifier messageCommit { "commit" }, messageName { "name" }, messageNote { "note" }, messageChanged { "changed" };

    // files.xml: FILES, with a FILE for each audio file read, its path, size and sha256 as a version's, and when it was last
    // written (modified: milliseconds since 1970), by which a file read before is known again.
    inline const juce::Identifier FILES { "FILES" };
    inline const juce::Identifier fileModified { "modified" };
} // namespace versions

//==============================================================================
// A merge taken into the song that waits for its commit (docs/design/versions.md: Merging; git's MERGE_HEAD), kept in the
// song so that it goes wherever the song goes: into its project file, its autosave, the song left on a branch and Local
// History, and out of it again with the Undo of the merge. Not part of a commit's song (VersionHistory::saveVersion() leaves
// it out), nor of the differences, which list only what the song holds; but a change to the project like any other. All its
// values are text, read by the versions as untrusted (model/versions/internal/VersionHistory.h: readMergeState()), which
// leave out a merge that doesn't hold together. song

// Child of PHAZITE, while a merge waits: what it merged and where, and which value of each of its conflicts the song has.
// (MERGING, with the elements above.)

// On MERGING: the commit merged in (version), and the current branch's last commit as the merge was taken (head), each a
// commit's id (64 lowercase hexadecimal digits). The merge waits until a commit of the branch's line finishes it (its
// commit: one with the one merged in as its second parent, its first following head, recording the IDs it gave), and
// while the branch's line doesn't hold the one merged in otherwise; its commit follows the branch's last commit then and
// the one merged in. Nothing is written into it after it is taken.
inline const Property<juce::String> mergingVersion { "version", on (MERGING), Kind::song };
inline const Property<juce::String> mergingHead { "head", on (MERGING), Kind::song };

// On MERGING: the song as it was before the merge was taken (before: a song piece in the project's Versions folder, view
// state included), which Cancel Merge makes the song again.
inline const Property<juce::String> mergingBefore { "before", on (MERGING), Kind::song };

// On MERGING: which value of each of the merge's conflicts the song has, a digit each in their order: 0 the parting's, 1
// this branch's, 2 the other line's. None: each this branch's.
inline const Property<juce::String> mergingChosen { "chosen", on (MERGING), Kind::song };

// On MERGING: the IDs the merge gave anew, which its commit records (renumbered); those the parting's and the other line's
// songs were translated through before it (translated; model/versions/internal/SongMerge.h: findTranslation()); and those
// the other line's song was translated through before that, where that line's own merges gave anew what this line holds too
// (a merge back, or another computer's merge of a version this line merged; crossed: model/versions/internal/VersionHistory.h,
// crossRenumbering()), a note's by its clip as they number it. Each separated by commas,
// each "from>to", or for a note "<its clip's group>/from>to" ("clip 66/3>7"), as a commit's RENUMBERED. No ID 0, and no two
// of one group with the same from, or the same to.
inline const Property<juce::String> mergingRenumbered { "renumbered", on (MERGING), Kind::song };
inline const Property<juce::String> mergingTranslated { "translated", on (MERGING), Kind::song };
inline const Property<juce::String> mergingCrossed { "crossed", on (MERGING), Kind::song };

// On PHAZITE: where the song stands in the project's versions (docs/design/versions.md: The song tied to its version): the
// branch it is on (versionsBranch: the branch's id, 32 lowercase hexadecimal digits) and the version it grew from
// (versionsBase: the version's id, 64 lowercase hexadecimal digits), which its changes are worked out against; none before
// the first version. They go wherever the song goes: its project file, its autosave and backups, the song left on a branch
// and Local History; a version's own song holds neither (VersionHistory::saveVersion() leaves them out). Text of any other
// form names nothing, and the loader leaves it out (model/project/internal/Project.h: makeConsistent()). No change to the
// song (isUncountedProperty()): written without undo steps, but by Take the Newer Versions, which moves the song to newer
// versions as one undo step whose Undo puts them back; a project whose song has no unsaved change has them written into its
// file as they move (model/project/internal/Project.h). Names that point at nothing the Versions folder holds (a project
// opened without it, a version that can't be read) leave the versions to take the branch the folder names and its last
// version (model/versions/internal/VersionHistory.h). state
// How many lowercase hexadecimal digits each holds, and whether a text is an id of that form (isVersionsID(), below): the
// one check of them, which the loader and the versions use.
inline constexpr int versionsBranchDigits = 32, versionsBaseDigits = 64;
inline const Property<juce::String> versionsBranch { "versionsBranch", on (PHAZITE), hexDigits (versionsBranchDigits), Kind::state,
                                                     uncounted };
inline const Property<juce::String> versionsBase { "versionsBase", on (PHAZITE), hexDigits (versionsBaseDigits), Kind::state, uncounted };

inline bool isVersionsID (const juce::String& text, int digits)
{
    return text.length() == digits && text.containsOnly ("0123456789abcdef");
}

// Whether a property of the song's own element is one of those two.
inline bool isVersionsProperty (const juce::ValueTree& tree, const juce::Identifier& p)
{
    return tree.hasType (PHAZITE) && (p == versionsBranch || p == versionsBase);
}

//==============================================================================
// The song's sections (model/arrangement/internal/Sections.h; docs/design.md, section 6): named, colored parts of the
// song over every track. song

// Child of PHAZITE, added with the first section (and kept once the last goes): the sections (SECTION), in no particular
// order. A song's sections are a value beside its tree while it is open (model/base/internal/SectionsValue.h), each property
// below as its description reads it, and its tree's SECTIONS an empty placeholder, kept where the song has one: so the song
// keeps no property or element Phazite doesn't know on a SECTIONS or a SECTION.
// (SECTIONS, with the elements above.)

// Child of SECTIONS: a section. Sections don't overlap.
// (SECTION, with the elements above.)

// On SECTION. int64, more than 0: its ID (SectionID), which the song gives it as it gives its tracks and clips theirs
// (Song::makeID()), so that no other item of the project has it.
inline const Property<std::int64_t> sectionID { "id", on (SECTION), Form::ownID, Kind::song };

// On SECTION. double, 0 or more: where it starts, in beats from the song's start.
inline const Property<double> sectionStart { "start", on (SECTION), within (0.0, maxBeats).orElse (0.0), Kind::song };

// On SECTION. double, more than 0: how long it lasts, in beats. A section written without it (or with none) opens 4 beats long.
inline const Property<double> sectionLength { "length", on (SECTION), within (moreThanZero, maxBeats).orElse (4.0), Kind::song };

// On SECTION: its name.
inline const Property<juce::String> sectionName { "name", on (SECTION), Kind::song };

// On SECTION: what it does in the song, such as "intro", "verse" or "drop", or none.
inline const Property<juce::String> sectionRole { "role", on (SECTION), Kind::song };

// On SECTION: a one-line suggestion of what to put in it, or none.
inline const Property<juce::String> sectionTip { "tip", on (SECTION), Kind::song };

// On SECTION. float 0..1, default 0.5: how intense it is, as the inspector shows it. Always written, as every section has one.
inline const Property<float> sectionEnergy { "energy", on (SECTION), within (0.0, 1.0), byDefault (0.5f), Kind::song };

// On SECTION, if it was given a color of its own: the color, as juce::Colour::toString() writes it. Without it, its
// role's.
inline const Property<juce::String> sectionColour { "colour", on (SECTION), Form::colour, Kind::song };

//==============================================================================
// The chord lane (model/chords/ChordModel.h; docs/design.md, section 14): the song's chords, each a Roman numeral in the
// key there, so that the chords follow when the key changes. song

// Child of PHAZITE, added when the first chord or key is written: the chords (CHORD) and the keys they are in (CHORDKEY),
// in no particular order, written chords first, then keys, each kind in the lane's order. Without one, the song's keys are
// the key set last (LASTKEY), and a lane made starts with it. A song's chords and keys are a value beside its tree while it
// is open (model/base/internal/ChordLaneValue.h), each property below as its description reads it, and its tree's
// CHORDLANE an empty placeholder that says it has a lane: so the song keeps no property or element Phazite doesn't know on
// a CHORDLANE, a CHORD or a CHORDKEY, and writes a chord's accidental and inversion only while they aren't 0.
// (CHORDLANE, with the elements above.)

// Child of CHORDLANE: a chord over a stretch of the song. Chords don't overlap.
// (CHORD, with the elements above.)

// On CHORD and CHORDKEY. double, 0 or more: where it starts, in beats from the song's start.
inline const Property<double> chordStart { "start", on (CHORD, CHORDKEY), within (0.0, maxBeats).orElse (0.0), Kind::song };

// On CHORD. double, more than 0: how long it lasts, in beats.
inline const Property<double> chordLength { "length", on (CHORD), within (moreThanZero, maxBeats).orElse (4.0), Kind::song };

// On CHORD. int 1..7: the step of the key's scale its root is on (Numeral::degree in model/chords/ChordValues.h).
inline const Property<int> chordDegree { "degree", on (CHORD), whole (1.0, 7.0).orElse (1.0), Kind::song };

// On CHORD. int -1, 0 or 1, default 0: the root a semitone under (-1) or over (1) that step, as in "bVII".
inline const Property<int> chordAccidental { "accidental", on (CHORD), whole (-1.0, 1.0), byDefault (0), Kind::song };

// On CHORD: its kind, "major", "minor", "dominantSeventh", "majorSeventh", "minorSeventh", "suspendedSecond",
// "suspendedFourth", "diminished", "augmented", "majorSixth", "minorSixth", "dominantNinth", "majorNinth", "power",
// "halfDiminishedSeventh", "diminishedSeventh" or "minorNinth" (getSavedChordTypeName()).
inline const Property<juce::String> chordType { "type", on (CHORD), Kind::song };

// On CHORD. int, default 0: which of its notes is lowest, 0 for its root, 1 for the next one up, and so on.
inline const Property<int> chordInversion { "inversion", on (CHORD), whole (0.0, 16.0), byDefault (0), Kind::song };

// Child of CHORDLANE: the key the chords are in from where it starts (chordStart) to the next one's start, with
// keyRoot and keyScale. The first starts at 0; a lane without any is in C major.
// (CHORDKEY, with the elements above.)

//==============================================================================
// The song's clips (model/arrangement/internal/Clips.h), each on its track's element: its MIDI clips with their notes,
// and its audio clips with the part of their files each plays and how. song

// Child of a TRACK that holds clips (trackIsTrack): a clip on it. A track's clips have an order, its list of them; a file's
// CLIP elements under a TRACK are read in their order, which becomes the list's, and written first among the track's
// children, in the list's order. A song's clips are values beside its tree while it is open (model/base/internal/
// ClipsValue.h: ClipValue), each property below as its description reads it and the whole kept within what a clip holds
// (keepWithinClip(): a file's end not before its start, a MIDI clip at least shortestClipBeats long, a take among its
// takes), so the song keeps no property or element Phazite doesn't know on a CLIP, nor one of the other kind of clip's (a
// length on an audio clip, a file on a MIDI clip), nor a CLIP anywhere but directly on such a TRACK, and writes each
// property only while it is set: one another file held at its default is gone at the next save.
// (CLIP, with the elements above.)

// On CLIP. int64, more than 0: its ID (ClipID), which the song gives it (Song::makeID()).
inline const Property<std::int64_t> clipID { "id", on (CLIP), Form::ownID, Kind::song };

// On CLIP: what it holds: clipIsMidi, notes, or clipIsAudio, a part of an audio file.
inline constexpr const char* clipIsMidi = "midi";
inline constexpr const char* clipIsAudio = "audio";
inline const Property<juce::String> clipKind { "kind", on (CLIP), oneOf (clipIsMidi, clipIsAudio), always, Kind::song };

// On CLIP, or none: its name. A clip without one (a new MIDI clip, at first) is called after its track, whatever the track is
// called (model/arrangement/internal/Clips.h: getClipName()).
inline const Property<juce::String> clipName { "name", on (CLIP), Kind::song };

// On CLIP. double, 0 or more: where it starts, in beats from the song's start. On a MIDI CLIP, double, more than 0: how long
// it lasts, in beats (an audio clip lasts as long as the part of its file it plays takes to play).
inline const Property<double> clipStart { "start", on (CLIP), within (0.0, maxBeats).orElse (0.0), Kind::song };
inline const Property<double> clipLength { "length", on (CLIP), within (moreThanZero, maxBeats).orElse (4.0), Kind::song };

// On a MIDI CLIP. double, 0 or more, default 0: how far into its notes it starts, in beats. Its notes count from its content
// start, that many beats before its start: trimming its start or slipping it moves that.
inline const Property<double> clipOffset { "offset", on (CLIP), within (0.0, maxBeats), byDefault (0.0), Kind::song };

// On CLIP. bool, default false: it is muted.
inline const Property<bool> clipMute { "mute", on (CLIP), byDefault (false), Kind::song };

// On CLIP. int64, or none: the alias group it is in, which has an ID of the song's (Song::makeID()). The clips of a group
// hold the same notes, with the same IDs: an edit to one clip's notes is made to each (model/notes/internal/Notes.h), as FL
// Studio's pattern clips play one pattern. The audio clips of a group play one file alike: they share the file (clipFile, its
// path as written), its tempo (clipTempo), pitch (clipPitch), direction (clipReversed) and warp markers (WARP, in the same
// order), which an edit through one clip writes into each (model/arrangement/internal/AudioClips.h: shareTheSound()); each
// keeps its own place (clipStart, its track), the part of the file it plays (clipFileStart, clipFileEnd), gain, fades, mute,
// takes (clipTake) and name (Rename writes it into each, but a clip named as its track keeps none). As a song opens
// (makeConsistent()), a group holds together: a clip of another kind than the group's first clip's (in the song's order), or
// whose notes and controllers' values (every property of each, in the same order), or, an audio clip, whose file, shared
// values (each as read, its default where it has none) or warp markers differ from its, is made a clip of its own, its link
// removed and what it holds kept; and a group left with one clip loses its link.
inline const Property<std::int64_t> clipLink { "link", on (CLIP), Form::songID, Kind::song };

// Child of a MIDI CLIP: a note, in no particular order; written before the clip's CONTROL elements. A clip's notes and
// controllers' values are values beside the song's tree while it is open (model/base/internal/SongValues.h), so the song
// keeps no property or element Phazite doesn't know on a NOTE or a CONTROL, as it keeps one elsewhere.
// (NOTE, with the elements above.)

// On NOTE. int64, 1 or more: its ID (NoteID, model/base/IDs.h), which no other note of its clip has, so that selections,
// undo and scripts can name one note (decided 2026-10-14). A copy of a clip, an alias too, keeps its notes' IDs. A new
// note's comes from the song's IDs (Song::makeID()), so that it is none a note had before.
inline const Property<std::int64_t> noteID { "id", on (NOTE), Form::ownID, Kind::song };

// On NOTE. int 0..127: its pitch, as a MIDI note number.
inline const Property<int> notePitch { "pitch", on (NOTE), whole (0.0, 127.0).orElse (60.0), Kind::song };

// On NOTE. double: where it starts, in beats from its clip's content start; and double, at least 1/64: how long it lasts,
// in beats.
inline const Property<double> noteStart { "start", on (NOTE), within (-maxBeats, maxBeats).orElse (0.0), Kind::song };
inline const Property<double> noteLength { "length", on (NOTE), within (moreThanZero, maxBeats).orElse (1.0), Kind::song };

// On NOTE. int 1..127: how hard it is played.
inline const Property<int> noteVelocity { "velocity", on (NOTE), whole (1.0, 127.0).orElse (100.0), Kind::song };

// On NOTE. bool, default false: it is muted.
inline const Property<bool> noteMute { "mute", on (NOTE), byDefault (false), Kind::song };

// On NOTE. bool, default false: it is a slide note, which plays nothing itself but glides the notes sounding as it starts to
// its pitch over its length; and a portamento note, which glides in from the note played before it (NoteValues.h).
inline const Property<bool> noteSlide { "slide", on (NOTE), byDefault (false), Kind::song };
inline const Property<bool> notePortamento { "portamento", on (NOTE), byDefault (false), Kind::song };

// On NOTE. int 1..16, default 1 (not written for 1): the MIDI channel it plays on, as musicians count them: which part of
// a plug-in with several parts plays it. Phazite's own instruments play every channel alike.
inline const Property<int> noteChannel { "channel", on (NOTE), whole (1.0, 16.0), byDefault (1), Kind::song };

// Child of a MIDI CLIP: a controller event a recording took in with the notes, such as a sustain pedal or a bend, in MIDI's
// own terms: its kind, controlIsController (a MIDI controller: controlNumber 0..127),
// controlIsPitchWheel, controlIsChannelPressure, controlIsAftertouch (on the note controlNumber) or
// controlIsProgramChange (which plays: it sets a plug-in's program; its value is the program, 0..127); where it is, in
// beats from its clip's content start; and its value, 0..127, or 0..16383 for the pitch wheel.
// (CONTROL, with the elements above.)
inline constexpr const char* controlIsController = "controller";
inline constexpr const char* controlIsPitchWheel = "pitchWheel";
inline constexpr const char* controlIsChannelPressure = "channelPressure";
inline constexpr const char* controlIsAftertouch = "aftertouch";
inline constexpr const char* controlIsProgramChange = "programChange";
inline const Property<juce::String> controlKind { "kind", on (CONTROL),
                                                  oneOf (controlIsController, controlIsPitchWheel, controlIsChannelPressure,
                                                         controlIsAftertouch, controlIsProgramChange),
                                                  always, Kind::song };
inline const Property<int> controlNumber { "number", on (CONTROL), whole (0.0, 127.0).orElse (0.0), Kind::song };
inline const Property<double> controlBeat { "beat", on (CONTROL), within (-maxBeats, maxBeats).orElse (0.0), Kind::song };
inline const Property<int> controlValue { "value", on (CONTROL), whole (0.0, 16383.0).orElse (0.0), Kind::song };

// On CONTROL. int 1..16, default 1 (not written for 1): the MIDI channel it is on, as musicians count them.
inline const Property<int> controlChannel { "channel", on (CONTROL), whole (1.0, 16.0), byDefault (1), Kind::song };

//==============================================================================
// On an audio CLIP (model/arrangement/internal/AudioClips.h). One that follows the song's tempo, or is pitched or reversed,
// plays a render of its file that Phazite makes itself (roadmap step 2.18; model/arrangement/internal/ClipRenders.h).

// The audio file it plays: a path relative to the project's file, with / between its parts whatever the system, or a full
// path.
inline const Property<juce::String> clipFile { "file", on (CLIP), Kind::song };

// double, 0 or more: the part of the file it plays, from and to, in seconds of the file as it was recorded, from < to,
// whichever way it plays.
inline const Property<double> clipFileStart { "fileStart", on (CLIP), within (0.0, maxSeconds).orElse (0.0), Kind::song };
inline const Property<double> clipFileEnd { "fileEnd", on (CLIP), within (0.0, maxSeconds).orElse (0.0), Kind::song };

// double 20..300 (AudioClipModel.h: minClipTempo..maxClipTempo), or none: the tempo the file was played at, in beats per
// minute, while the clip stretches to follow the song's; none while it plays at the file's own speed.
inline const Property<double> clipTempo { "tempo", on (CLIP), within (20.0, 300.0), Kind::song };

// float -48..48, default 0: how many semitones it is pitched up or down, with cents as hundredths.
inline const Property<float> clipPitch { "pitch", on (CLIP), within (-48.0, 48.0), byDefault (0.0f), Kind::song };

// true, or none: it plays its file backwards.
inline const Property<bool> clipReversed { "reversed", on (CLIP), Kind::song };

// float, default 0: how much louder or quieter it plays, in decibels.
inline const Property<float> clipGain { "gain", on (CLIP), within (-60.0, 24.0), byDefault (0.0f), Kind::song };

// double, 0 or more, default 0: how long it fades in, and out, in seconds; the shapes of the fades (PlaylistModel.h:
// FadeCurve), fadeIsCurve or fadeIsSCurve, default a curve; and how far each bends, a float from -1 to 1, default 0 (straight).
inline const Property<double> clipFadeIn { "fadeIn", on (CLIP), within (0.0, maxSeconds), byDefault (0.0), Kind::song };
inline const Property<double> clipFadeOut { "fadeOut", on (CLIP), within (0.0, maxSeconds), byDefault (0.0), Kind::song };
inline constexpr const char* fadeIsCurve = "curve";
inline constexpr const char* fadeIsSCurve = "sCurve";
inline const Property<juce::String> clipFadeInShape { "fadeInShape", on (CLIP), oneOf (fadeIsCurve, fadeIsSCurve), byDefault (fadeIsCurve),
                                                      Kind::song };
inline const Property<juce::String> clipFadeOutShape { "fadeOutShape", on (CLIP), oneOf (fadeIsCurve, fadeIsSCurve),
                                                       byDefault (fadeIsCurve), Kind::song };
inline const Property<float> clipFadeInBend { "fadeInBend", on (CLIP), within (-1.0, 1.0), byDefault (0.0f), Kind::song };
inline const Property<float> clipFadeOutBend { "fadeOutBend", on (CLIP), within (-1.0, 1.0), byDefault (0.0f), Kind::song };

// Child of an audio CLIP that follows the song's tempo, in no particular order: a warp marker (AudioClipModel.h), which pins a
// time of the file to where it plays, the audio between two of them stretched to fit, as Ableton Live's warp markers do. On
// WARP. double, 0 or more: its time in the file, in seconds as it was recorded; and double: where that time plays, as a time
// of the file played straight at the clip's tempo (clipTempo), in seconds, so that the markers keep their beats as the clip's
// tempo changes. song
// (WARP, with the elements above.)
inline const Property<double> warpTime { "time", on (WARP), within (0.0, maxSeconds).orElse (0.0), Kind::song };
inline const Property<double> warpPlaysAt { "playsAt", on (WARP), within (-maxSeconds, maxSeconds).orElse (0.0), Kind::song };

//==============================================================================
// Recording audio (model/transport/RecordingModel.h; roadmap step 3.4).

// A track's input and how it is heard: trackInput and trackMonitoring, with the tracks above.

// On PHAZITE. The input chosen last on any track (an id as trackInput's), which the next track's list shows first. state
inline const Property<juce::String> lastInput { "lastInput", on (PHAZITE), Kind::state, uncounted };

// On PHAZITE. bool, default false: Record punches in and out at the looped range. Like the loop's switch, not an unsaved
// change. state
inline const Property<bool> punch { "punch", on (PHAZITE), byDefault (false), Kind::state, uncounted };

// On an audio CLIP recorded round a looped range, whose file holds a take for each time round, one after another
// (ClipTakes). song
// int, 2 or more: how many takes the file holds.
inline const Property<int> clipTakes { "takes", on (CLIP), whole (2.0, 1.0e6), Kind::song };

// int, 1..clipTakes: the take the clip plays.
inline const Property<int> clipTake { "take", on (CLIP), whole (1.0, 1.0e6), Kind::song };

// double, more than 0: how long each take is, in seconds: the looped range's length when it was recorded.
inline const Property<double> clipTakeLength { "takeLength", on (CLIP), within (moreThanZero, maxSeconds), Kind::song };

// Phazite's drum sampler (devices/drums/DrumSampler.h) keeps in its DEVICE's SOUNDS what each pad's layers play, its pad
// named by its note (0 to 127): "pad36Sound" names one of its factory sounds for the first layer (devices/drums/
// FactoryDrums.h: "808 Kick"; with none, or a name that is none of theirs, the pad's own in the default kit, and nothing
// off the default kit's sixteen pads), and "pad36SoundFile" a sound file it plays instead while it is there: a path
// relative to the project's folder ("Audio/Kick 01.wav"), or in a preset's file to the preset's ("Kit Sounds/Kick 01.wav");
// "pad36Sound2" to "pad36Sound4" and "pad36SoundFile2" to "pad36SoundFile4" the same for its other layers, which play nothing
// without them.
inline constexpr int drumPads = 128, drumLayers = 4; // as devices::DrumSampler::numPads and numLayers

// "pad36Sound", or for another layer than the first, "pad36Sound2", after a pad's note and its layer (from 0).
inline std::array<juce::Identifier, drumPads * drumLayers> makePadSoundNames (const juce::String& kind)
{
    std::array<juce::Identifier, drumPads * drumLayers> made;

    for (int pad = 0; pad < drumPads; ++pad)
        for (int layer = 0; layer < drumLayers; ++layer)
            made[(size_t) (pad * drumLayers + layer)] =
                "pad" + juce::String (pad) + kind + (layer > 0 ? juce::String (layer + 1) : juce::String());

    return made;
}

// The two families, each pad's layers in order.
inline const Family padSounds { "pad<note>Sound<layer>", makePadSoundNames ("Sound"), on (SOUNDS), Kind::song };
inline const Family padSoundFiles { "pad<note>SoundFile<layer>", makePadSoundNames ("SoundFile"), on (SOUNDS), Kind::song };

inline const juce::Identifier& getPadSound (int pad, int layer = 0)
{
    return padSounds[(size_t) (juce::jlimit (0, drumPads - 1, pad) * drumLayers + juce::jlimit (0, drumLayers - 1, layer))];
}

inline const juce::Identifier& getPadSoundFile (int pad, int layer = 0)
{
    return padSoundFiles[(size_t) (juce::jlimit (0, drumPads - 1, pad) * drumLayers + juce::jlimit (0, drumLayers - 1, layer))];
}

// Phazite's synth (devices/synth/Synth.h) keeps in its DEVICE's SOUNDS what each of its two
// oscillators plays: "osc1Wavetable" and "osc2Wavetable" name one of its factory tables (devices/wavetable/
// FactoryWavetables.h: "Vowels"; with none, or a name that is none of theirs, the first, "Basic Shapes"), and
// "osc1WavetableFile" and "osc2WavetableFile" a sound file it plays instead while they are there: a path relative to the
// project's folder ("Audio/Growl.wav"), or in a preset's file to the preset's. osc counts from 0.
inline const Family oscillatorWavetables { "osc<osc>Wavetable", juce::StringArray { "osc1Wavetable", "osc2Wavetable" }, on (SOUNDS),
                                           Kind::song };
inline const Family oscillatorWavetableFiles { "osc<osc>WavetableFile", juce::StringArray { "osc1WavetableFile", "osc2WavetableFile" },
                                               on (SOUNDS), Kind::song };

inline const juce::Identifier& getOscillatorWavetable (int osc)
{
    return oscillatorWavetables[(size_t) juce::jlimit (0, 1, osc)];
}

inline const juce::Identifier& getOscillatorWavetableFile (int osc)
{
    return oscillatorWavetableFiles[(size_t) juce::jlimit (0, 1, osc)];
}

// Phazite's sampler (devices/sampler/Sampler.h) keeps in its DEVICE's SOUNDS what it plays: "sample" names one of its
// factory sounds (devices/sampler/FactorySamples.h: "808 Bass"; with none, or a name that is none of theirs, the first),
// "sampleFile" a sound file it plays instead while it is there: a path relative to the project's folder ("Audio/Vox.wav"),
// or in a preset's file to the preset's; "sampleSlices" the slices put in by hand, which stay whatever slices it, their starts as
// parts of the sound's length from 0 to 1, in order, a space between them ("0.125 0.25"); and "sampleSlicesRemoved" where
// slices it found were taken away by hand, the same way. None for none.
inline const Property<juce::String> samplerSample { "sample", on (SOUNDS), Kind::song };
inline const Property<juce::String> samplerSampleFile { "sampleFile", on (SOUNDS), Kind::song };
inline const Property<juce::String> samplerSlices { "sampleSlices", on (SOUNDS), Form::numbers, within (0.0, 1.0), Kind::song };
inline const Property<juce::String> samplerSlicesRemoved { "sampleSlicesRemoved", on (SOUNDS), Form::numbers, within (0.0, 1.0),
                                                           Kind::song };

// What of a SOUNDS element names a sound file, rather than a factory sound or a table: a pad's file, an oscillator's or the
// sampler's.
inline bool isSoundFileProperty (const juce::Identifier& property)
{
    for (int pad = 0; pad < drumPads; ++pad)
        for (int layer = 0; layer < drumLayers; ++layer)
            if (property == getPadSoundFile (pad, layer))
                return true;

    return property == getOscillatorWavetableFile (0) || property == getOscillatorWavetableFile (1) || property == samplerSampleFile;
}

//==============================================================================
// Presets (model/presets/PresetModel.h): files of their own (".phazitepreset"), outside any project, in JSON, in Phazite's
// own names: devices by the type and parameter IDs of their descriptions (src/devices/), plug-ins by who they are and the
// settings they give themselves, and containers, modulators and macros as Phazite names them. The model makes the song's
// elements from them as a preset loads (model/presets/internal/Presets.h). No format version (section 18 of
// docs/design.md: none while Phazite is pre-alpha).
//
//   { "kind": "device", "device": DEVICE }            an instrument or an effect
//   { "kind": "container", "container": CONTAINER }   a container of effects, or an instrument container
//   { "kind": "track", "track": TRACK }               a track's devices, modulators and mix
//
//   DEVICE, one of Phazite's own: { "type": its type ID, "name", "instrument": true or false,
//       "parameters": { parameter ID: value, ... }, every one of them, its mix too,
//       "sounds": { what names a file, a factory sound, a table or slices (getPadSound(), getPadSoundFile(),
//                   getOscillatorWavetable(), getOscillatorWavetableFile(), samplerSample, samplerSampleFile,
//                   samplerSlices, samplerSlicesRemoved): a file's path from the preset's folder (the file in a folder
//                   beside it, named after it: "Kit Sounds/Boom.wav"), the factory sound's or table's name, or the
//                   slices } }
//     or a plug-in: { "plugin": { "format", "name", "manufacturer", "identifier", "uid" }, "name", "instrument",
//       "state": its own settings, in base 64, "program": its program, "mix": 0 to 1 }; its identifier the file's name alone
//       for a plug-in under the user's home folder
//   CONTAINER: { "name", "mix": 0 to 1, "instrument": true for an instrument container, "split": what containerSplit
//                names (none for layers), "chains": [ CHAIN, ... ], "macros": [ MACRO, ... ] }
//   CHAIN: { "name", "volume": in decibels, "pan": -1 to 1, "mute", "solo", "crossover": in hertz (a band's),
//            "lowKey", "highKey", "lowVelocity", "highVelocity" (an instrument container's), "devices": [ DEVICE, ... ] }
//   MACRO: { "name", "value": 0 to 1, "modulates": [ TARGET, ... ] }
//   TRACK: { "name", "colour": its ARGB in hex, "volume": in decibels, "pan": -1 to 1, "drumMode", "drumOrder",
//            "drumHidden" (its drum rows' view state, as the project keeps them), "instrument": DEVICE or
//            { "container": CONTAINER }, "effects": [ DEVICE or { "container": CONTAINER }, ... ],
//            "modulators": [ MODULATOR, ... ], "macros": [ MACRO, ... ] }
//   MODULATOR: { "type": one of modulatorTypeNames, "parameters": { parameter ID: value, ... },
//                "steps": [ value, ... ] (a step sequencer's), "modulates": [ TARGET, ... ] }
//   TARGET: what a modulator or a macro modulates, by an amount from -1 to 1: { "parameter": ID, "amount", and where:
//           a container's macro's, "chain": its chain's index in the container, with "device": its index among the chain's
//           devices, or none for the chain's own volume, pan or crossover; a track's, "instrument": true, or "effect": its
//           index among the track's effects (with "chain" and "inside", a device's index in that chain, for what is in a
//           container, as a container's macro names it), or "mixer": true with "parameter" "volume" or "pan" }
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
    inline const juce::Identifier chains { "chains" }, chain { "chain" }, split { "split" }, mute { "mute" }, solo { "solo" },
        crossover { "crossover" }, lowKey { "lowKey" }, highKey { "highKey" }, lowVelocity { "lowVelocity" },
        highVelocity { "highVelocity" };
    inline const juce::Identifier colour { "colour" }, volume { "volume" }, pan { "pan" }, drumMode { "drumMode" },
        drumOrder { "drumOrder" }, drumHidden { "drumHidden" }, effects { "effects" }, modulators { "modulators" }, steps { "steps" };

    // What "kind" says.
    inline constexpr const char* deviceKind = "device";
    inline constexpr const char* containerKind = "container";
    inline constexpr const char* trackKind = "track";

    // What a modulator's "type" says, in ModulatorType's order (model/modulation/ModulationValues.h), macros left out: the
    // words of the song's modulatorType.
    inline constexpr const char* modulatorTypeNames[] = { modulatorIsLfo,    modulatorIsEnvelopeFollower, modulatorIsSteps,
                                                          modulatorIsRandom, modulatorIsBreakpoints,      modulatorIsNoteFollower };
} // namespace preset
} // namespace phazite::schema

namespace phazite
{
// The largest ID a song counts in a part of it, below a bound (schema::maxID, or schema::maxCountedID as the song is given
// its IDs anew): its items' (each kind's is named "id": trackID, clipID, noteID, deviceID, chainID, sendID, modulatorID,
// sectionID), its channels' faders and VCAs, and its alias groups (clipLink), where its elements hold them
// (schema::holdsID()). Inside an element of a type Phazite doesn't know (schema::isKnownType()), which the song keeps as
// it is, only those below schema::maxCountedID: counted, so that no ID the song gives equals one a later Phazite would
// find in there; but one at or past it, which the loader can't give anew, never raises the song's count nor makes it give
// its IDs anew. The one count of them: the song's (findNextID()), the loader's (makeConsistent()), and the rule below.
std::uint64_t findLargestID (const juce::ValueTree&, std::uint64_t below = schema::maxID);

// Whether a song is given its IDs at or past schema::maxCountedID anew as it loads: the largest ID it counts
// (findLargestID()), or its next ID, at or past schema::renumberMark. The loader (makeConsistent()) and the versions,
// which refuse a commit whose song's IDs are given anew (model/versions/SongPieces.cpp), ask this one rule.
bool mustGiveIDsAnew (const juce::ValueTree& song);
bool mustGiveIDsAnew (std::uint64_t largestID, std::uint64_t nextID);

// Whether two songs' IDs are in one numbering (schema::idNumbering): neither given anew, or both by the same renumbering.
// What the versions ask before they match a version's items with the song's by ID (model/versions/VersionsModel.cpp).
bool areNumberedAlike (const juce::ValueTree& song, const juce::ValueTree& other);

// The ID a song gives next: past its count (nextID) and every ID it holds. Of a song's tree (a version's, a merge's, a whole
// copy): what the tree holds. Of a live song: its tree's and what its values hold beside it (IDsHeld.h: findLargestIDHeld()),
// which its tree no longer does (roadmap step S5.20); what everything that works out a live song's next ID asks.
std::uint64_t findNextID (const juce::ValueTree& song);
std::uint64_t findNextID (const Song&);

// Brings the song's count there (findNextID()). As a project opens and saves.
void countIDs (Song&);

// A copy of a part of the song (a track's element, a folder's with its tracks, a device's) made the song's own: every item
// in it given a new ID of the song's (tracks, their faders and VCAs, clips, devices, containers, sends, modulators and
// macros; not notes, which a clip's copy keeps, as its aliases do, nor alias groups, which the copies join), and what in it
// names them (a lane's device, a modulation's) renamed to match. Returns each old ID with its new one, for what outside
// the copy names them (the song's automation: model/automation/internal/Automation.h, copyCurves()).
using CopiedIDs = std::map<std::uint64_t, std::uint64_t>;
CopiedIDs giveNewIDs (Song&, juce::ValueTree& copy);

// An ID as a property holds or names it (schema::holdsID(), namesID()), and another written as that one was: a device that
// a lane, a curve or a modulation names is text ("1042"), the other IDs numbers.
std::uint64_t readID (const juce::var&);
juce::var writeIDLike (const juce::var& old, std::uint64_t id);
} // namespace phazite
