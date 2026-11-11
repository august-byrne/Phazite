// SPDX-License-Identifier: AGPL-3.0-or-later
// Everything Phazite writes into a project file: the names of its elements, properties and values, each defined once,
// here, with where it is stored and what it holds. Code reads and writes them only through these names.
//
// Kinds: song = part of the song, changed in undo steps; view = how the project is shown, saved but never an undo
// step; state = remembered by the project, not an undo step.
//
// Write the names qualified, as schema::outline, never with "using namespace schema": they would clash with
// parameters and variables of the same name.
#pragma once

#include "model/base/Song.h"

#include <algorithm>
#include <array>
#include <map>
#include <vector>

namespace phazite::schema
{
//==============================================================================
// The song's own element, and what it keeps for the whole song (Song::getState()).

// The song (model/base/Song.h): the element a project file holds, with everything below in it. song
inline const juce::Identifier PHAZITE { "PHAZITE" };

// On PHAZITE. int64, above every ID in the song: the next ID it gives an item (Song::makeID()), each kind from the
// same numbers, never one twice. Written without an undo step, and no change of its own (the item it numbers is). state
inline const juce::Identifier nextID { "nextID" };

// On PHAZITE. bool, default true: the loop follows the song (getSongLoopRange()); false while a range is looped on
// purpose. Written only when it changes. state
inline const juce::Identifier loopFollowsSong { "loopFollowsSong" };

// The transport's state that the song keeps (model/transport/internal/Playback.h, Metronome.h). None is an undo step.
// On PHAZITE:
// - loop: bool, default false: playback loops, around the song while the loop follows it, or else around the range
//   looped on purpose, loopStart to loopEnd (double, in beats from the song's start; none while the loop follows the song);
// - playStart: double, in beats, default 0: where playback starts, the song's start again as a project opens;
// - metronome: bool, default false: the metronome clicks while the song plays. state
inline const juce::Identifier loop { "loop" };
inline const juce::Identifier loopStart { "loopStart" };
inline const juce::Identifier loopEnd { "loopEnd" };
inline const juce::Identifier playStart { "playStart" };
inline const juce::Identifier metronome { "metronome" };

// On PHAZITE. The id of the song outline applied last (Outline::id), or none. song
inline const juce::Identifier outline { "outline" };

// On PHAZITE. bool, default false: a change of the song's tempo leaves its patterns (MIDI clips) at their speed, their
// notes keeping their times, rather than on their beats (model/transport/internal/Tempo.h). song
inline const juce::Identifier patternsKeepTheirSpeed { "patternsKeepTheirSpeed" };

// On PHAZITE. The project's notes, as the Project view shows them (model/project/ProjectContents.h): text, its lines
// split by "\n"; none while there are none. song
inline const juce::Identifier notes { "notes" };

// Child of PHAZITE: the key set last on any track (keyRoot and keyScale), which new tracks get. state
inline const juce::Identifier LASTKEY { "LASTKEY" };

//==============================================================================
// The song's tempo map (model/transport/internal/Tempo.h): its tempos and time signatures, each from a beat on, which
// everything in the song is placed on. Everything in the song counts beats, quarter notes whatever the time signature, and a
// tempo counts them a minute; a time signature sets only how long a bar is and which note its own beats count
// (model/transport/TempoValues.h). The playlist's tempo lane adds tempos along the song, each held until the next or
// ramping to it; for now a song has one time signature, from its start. song

// Child of PHAZITE, which every project has (Project adds it as the project opens): the song's tempos (TEMPO) and time
// signatures (TIMESIGNATURE), in no particular order. Without one, the song is at 120 BPM in 4/4.
inline const juce::Identifier TEMPOMAP { "TEMPOMAP" };

// Child of TEMPOMAP: a tempo, from where it starts (mapBeat) to the next one's start, in quarter notes per minute (bpm, 20..300,
// to a hundredth), and how it goes on to the next (tempoCurve, float -1..1, as an automation point's pointCurve; without
// it, 1: held until the next tempo, which it jumps to; 0 ramps straight to the next, and between, the ramp bends). The
// first starts at 0.
inline const juce::Identifier TEMPO { "TEMPO" };
inline const juce::Identifier bpm { "bpm" };
inline const juce::Identifier tempoCurve { "curve" };

// Child of TEMPOMAP: a time signature, from where it starts (mapBeat), as many beats in a bar (numerator, int 1..32) of a
// note value (denominator: 2, 4, 8 or 16): its bars are numerator x 4 / denominator quarter notes long. The first starts at 0.
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
// The song's tracks (model/arrangement/internal/Tracks.h): the playlist's tracks and folders and the mixer's returns,
// top to bottom, each folder's tracks in it, with what each is, its name and color, its mute and solo, its key, its
// audio input and how it shows. song, but for what says it is view

// Child of PHAZITE, which every project has (Project adds it as the project opens): the song's tracks (TRACK), top to
// bottom.
inline const juce::Identifier TRACKS { "TRACKS" };

// Child of TRACKS, or of a folder's TRACK: a track.
inline const juce::Identifier TRACK { "TRACK" };

// On TRACK, and on MASTER. int64, more than 0: its ID (TrackID), which the song gives it (Song::makeID()).
inline const juce::Identifier trackID { "id" };

// On TRACK: what it is: trackIsTrack (a track, which holds clips), trackIsFolder (a folder, which holds tracks) or
// trackIsReturn (a return, which takes in what tracks send to it, and which no solo silences).
inline const juce::Identifier trackKind { "kind" };
inline constexpr const char* trackIsTrack = "track";
inline constexpr const char* trackIsFolder = "folder";
inline constexpr const char* trackIsReturn = "return";

// On TRACK: its name; empty or none for a track's number, "Track 3", as it is counted among the tracks and returns.
inline const juce::Identifier trackName { "name" };

// On TRACK: its color, as juce::Colour::toString() writes it; none for its row's in the palette (TrackColours.h).
inline const juce::Identifier trackColour { "colour" };

// On TRACK. bool, default false: it is muted; and it is soloed. Neither is an undo step (as in many DAWs). song
inline const juce::Identifier trackMute { "mute" };
inline const juce::Identifier trackSolo { "solo" };

// On TRACK, and on MASTER: how the track writes its automation as the song plays (AutomationMode): automationTouch,
// automationLatch or automationWrite; none = read, which plays its lanes and writes nothing. Changing it is no undo step,
// as mute and solo aren't. song
inline const juce::Identifier trackAutomationMode { "automationMode" };
inline constexpr const char* automationTouch = "touch";
inline constexpr const char* automationLatch = "latch";
inline constexpr const char* automationWrite = "write";

// On a folder's TRACK. true, or none: the folder was made a bus with the bus switch, so it stays one without effects.
inline const juce::Identifier folderBus { "bus" };

// On TRACK, and on MASTER: the channel's volume, in decibels (-100 for silence), as its fader sets it (a
// track's, a return's or a bus's; a plain folder's VCA), none for 0 dB, or for the master's -3 dB, a new song's level; and
// its pan, from -1 (left) to 1 (right), none for the centre, which a plain folder keeps for when it is a bus again.
inline const juce::Identifier channelVolume { "volume" };
inline const juce::Identifier channelPan { "pan" };

// On TRACK, and on MASTER: the device that sets the channel's volume, by its ID (DeviceID, of the song's), which the
// automation and the modulation of its volume and pan name. A track's, a return's, a bus's (a folder that mixes its
// tracks) and the master's is a fader (channelFader: its volume and pan, with its meter after it); a plain folder's is a
// VCA (channelVCA), which sets its tracks' volume. A folder that becomes a bus or plain again gets one of the other kind,
// with an ID of its own.
inline const juce::Identifier channelFader { "fader" };
inline const juce::Identifier channelVCA { "vca" };

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
inline const juce::Identifier trackFrozen { "frozen" };

// A key, on a TRACK (song), on LASTKEY (state) and on CHORDKEY (song): the root, int 0..11 (0 is C), none = no key; and the
// scale: "major", "minor", "dorian", "phrygian", "lydian", "mixolydian", "locrian", "harmonicMinor" or "melodicMinor".
inline const juce::Identifier keyRoot { "keyRoot" };
inline const juce::Identifier keyScale { "keyScale" };

// On TRACK (model/transport/RecordingModel.h; roadmap step 3.4). The audio device's input the track records (AudioInput::id):
// a channel, counted from 1 ("1"), or a pair of neighbours from an odd-numbered one ("1+2"); none for no input.
inline const juce::Identifier trackInput { "input" };

// On a TRACK that plays a drum sampler's pad on a mixer channel of its own (roadmap step 4.12;
// model/devices/internal/DrumKit.h), as FL Studio's FPC sends a pad to a mixer track: the sampler, by its device's ID
// (DeviceID, int64), and the pad, by its note (int, 0 to 127). The pad's sound leaves the sampler's track for it. None for
// any other track.
inline const juce::Identifier trackPadDevice { "padDevice" };
inline const juce::Identifier trackPad { "pad" };

// On TRACK. When the track's input is heard through it: monitorAlways, monitorNever, or none, while it is armed.
inline const juce::Identifier trackMonitoring { "monitoring" };
inline constexpr const char* monitorAlways = "always";
inline constexpr const char* monitorNever = "never";

// On TRACK. int 32..320, default 64: the track's height in the playlist, in pixels at an interface scale of 100%. view
inline const juce::Identifier trackHeight { "height" };

// On TRACK. bool, default false: the track is a thin row with only its name; a collapsed folder hides its tracks. view
inline const juce::Identifier trackCollapsed { "collapsed" };

// On TRACK. bool, default false: the track stays in view while the others scroll. view
inline const juce::Identifier trackPinned { "pinned" };

// On TRACK. bool: the piano roll shows the track's clips in drum mode; none = on while the track's instrument is
// Phazite's drum sampler. view
inline const juce::Identifier drumMode { "drumMode" };

// On TRACK: the order of the drum rows, as note numbers separated by commas, top first; rows it doesn't list come
// after them. view
inline const juce::Identifier drumOrder { "drumOrder" };

// On TRACK: the drum rows hidden, as note numbers separated by commas. view
inline const juce::Identifier drumHidden { "drumHidden" };

// Child of PHAZITE, which every project has: the master channel's own element, with its write mode
// (trackAutomationMode), its volume and pan (channelVolume, channelPan), its devices, and its automation lanes (LANE, view),
// with an ID of the song's (trackID) like a track's. song, but for its lanes
inline const juce::Identifier MASTER { "MASTER" };

// Child of TRACK and of MASTER, one for each automation lane the playlist shows under the track, in their order
// (docs/design.md, section 10): the automation of a parameter of one of the track's devices, its volume and pan included.
// A lane of a device that isn't on the track any more is ignored, and a parameter with automation but no lane here shows
// one after these. view
inline const juce::Identifier LANE { "LANE" };

// On LANE: the device, by its ID (DeviceID: "1042").
inline const juce::Identifier laneDevice { "device" };

// On LANE: the parameter, by its ID in its device, as its curve names it (curveParameter).
inline const juce::Identifier laneParameter { "parameter" };

// On LANE. int 32..320, default 48: the lane's height in the playlist while it isn't collapsed, in pixels at an interface
// scale of 100%.
inline const juce::Identifier laneHeight { "height" };

// On LANE. true, or none: the lane shows as a thin row.
inline const juce::Identifier laneCollapsed { "collapsed" };

//==============================================================================
// The song's devices (model/mixer/internal/Mixer.h): each track's, a folder's (as a bus) and the master's, in the order its
// sound goes through them, an instrument first, each with what it is set to, and each container with its devices. song

// Child of TRACK and of MASTER: a device. On DEVICE: its ID (DeviceID, of the song's: Song::makeID()); its type:
// the typeID of one of Phazite's own (src/devices/<area>/: "phaziteEq"), deviceIsPlugin for a plug-in, or the type of a
// device registered for the tests (model/devices/internal/Devices.h: registerDeviceType()); and deviceOff, bool, default
// false: it is switched off.
inline const juce::Identifier DEVICE { "DEVICE" };
inline const juce::Identifier deviceID { "id" };
inline const juce::Identifier deviceType { "type" };
inline constexpr const char* deviceIsPlugin = "plugin";
inline const juce::Identifier deviceOff { "off" };

// The format a device type of Phazite's own names in its description (model/mixer/DeviceType.h), as a plug-in's names
// "VST3", with its type ID as the description's identifier: what the browser, the menus and a drag carry; never saved.
inline constexpr const char* ownDeviceFormat = "Phazite";

// On a DEVICE: the track it listens to through its sidechain, by the track's ID (TrackID); none for none.
inline const juce::Identifier deviceSidechain { "sidechain" };

// On a DEVICE: where its window was, the left and the top of it on the screen (int, pixels), where it opens
// again; none for a window never moved. view
inline const juce::Identifier deviceWindowX { "windowX" };
inline const juce::Identifier deviceWindowY { "windowY" };

// Children of one of Phazite's own devices' DEVICE: VALUES, its parameters' values as they were set by hand (not as
// automation or a modulator moves them), each a property named by the parameter's ID in the device's description, none
// for one that was never set (at its default); and SOUNDS, the factory sounds, files, tables and slices it plays, as
// properties named by getPadSound(), getPadSoundFile(), getOscillatorWavetable(), getOscillatorWavetableFile(),
// samplerSample, samplerSampleFile, samplerSlices and samplerSlicesRemoved (below).
inline const juce::Identifier VALUES { "VALUES" };
inline const juce::Identifier SOUNDS { "SOUNDS" };

// On a plug-in's DEVICE: which plug-in it is, as scanning found it and a preset names it (preset::plugin):
// its format ("VST3"), name, maker (manufacturer), identifier (its file on the computer it was added on, or what its
// format finds it by) and unique ID (int: JUCE's number for it, which for a VST3 is made from its class ID). A song finds
// its plug-in by its format, unique ID and maker first, wherever this computer keeps its file
// (model/mixer/internal/Mixer.h: findKnownPlugin()), then by its file and name, then by its name.
inline const juce::Identifier pluginFormat { "format" };
inline const juce::Identifier pluginName { "name" };
inline const juce::Identifier pluginMaker { "manufacturer" };
inline const juce::Identifier pluginIdentifier { "identifier" };
inline const juce::Identifier pluginUid { "uid" };
// On a plug-in's DEVICE: true for an instrument, which turns notes into sound, as its description says; none for an effect.
inline const juce::Identifier pluginInstrument { "instrument" };

// On a plug-in's DEVICE and on a CONTAINER: its mix, 0 to 1, as setMix() sets it; none for 1.
inline const juce::Identifier deviceMix { "mix" };

// On a plug-in's DEVICE: what the plug-in keeps of its own, which the project stores outside the undo history as it saves,
// before it copies the plug-in or takes it away, and as a preset loads into it (model/plugins/internal/PluginInstances.h):
// its state, the bytes it keeps its settings in (in base 64, as RFC 4648 has it: model/base/internal/SongText.h), none for
// none, which for a VST3 are its format's own preset file, a .vstpreset (sandbox/core/Vst3State.h), as DAWproject holds a
// VST3's state too; and its program (int), for a plug-in with programs. state
inline const juce::Identifier pluginState { "state" };
inline const juce::Identifier pluginProgram { "program" };

// On a plug-in's DEVICE, stored with its state: its ID in its format, which other hosts know it by (a VST3's class ID, 32
// hexadecimal digits, as its preset file holds it), none for another format; and the version of the plug-in that made the
// state, as the plug-in tells it ("1.3.7"), none if it tells none. state
inline const juce::Identifier pluginFormatID { "formatID" };
inline const juce::Identifier pluginVersion { "version" };

// Children of a plug-in's DEVICE: the values set by hand of its parameters, those set by Phazite's controls, and those of
// the parameters that automation or a modulator moves away from them, stored with its state (its state holds the others),
// each a VALUE with the parameter's ID, the plug-in's own (a VST3's number, which can't name a property in a file:
// sandbox/core/PluginInfo.h), and its value set by hand (float, 0 to 1).
inline const juce::Identifier VALUE { "VALUE" };
inline const juce::Identifier valueParameter { "parameter" };
inline const juce::Identifier valueSetByHand { "value" };

// Child of TRACK and of MASTER, among its devices: a container (model/mixer/internal/Containers.h), with its ID, deviceOff
// and deviceMix, as a device has them, and its name. Its children are its chains (CHAIN), one at least, in their order, each
// with its devices, and its macros (MACRO), with what they modulate. A container of effects plays its chains side by side,
// each on its part of the sound that comes in (containerSplit), and adds up what they put out; an instrument container
// (containerInstrument), which is a track's instrument, plays its chains' instruments, each on the notes in its ranges.
inline const juce::Identifier CONTAINER { "CONTAINER" };
inline const juce::Identifier containerName { "name" };

// On a CONTAINER of effects: what part of the sound each chain plays: splitBands, a band of it, low to high in the chains'
// order, each from its chain's crossover (the first from the bottom); splitMidSide, the first its middle (what its sides
// share) and the second its side (what they don't); splitLeftRight, the first its left and the second its right; none for
// all of it, each chain a layer. The parts add up to the sound, so chains with nothing in them pass it as it came, but
// layers, which add up as many times.
inline const juce::Identifier containerSplit { "split" };
inline constexpr const char* splitBands = "bands";
inline constexpr const char* splitMidSide = "midSide";
inline constexpr const char* splitLeftRight = "leftRight";

// On a CONTAINER. true, or none: it is an instrument container, a track's instrument, each of whose chains holds an
// instrument first and the effects after it, and plays the notes in its ranges.
inline const juce::Identifier containerInstrument { "instrument" };

// Child of CONTAINER: a chain, in their order, its devices (DEVICE) its children in the order its sound goes through them. On
// CHAIN: its ID (DeviceID, of the song's), which its parameters' curves, lanes and modulations name; its name; its volume
// and pan (channelVolume, channelPan), as a channel's; chainMute and chainSolo, true or none, set as a track's mute and solo
// are, without undo steps; on a container split into bands, its chainCrossover, in hertz (float): where its band starts,
// the first chain's from the bottom, none for its place's default (model/mixer/internal/Containers.h:
// getDefaultCrossover()); and on an instrument container's, the notes it plays: chainLowKey and chainHighKey (int 0..127,
// none for 0 and 127) and chainLowVelocity and chainHighVelocity (int 1..127, none for 1 and 127).
inline const juce::Identifier CHAIN { "CHAIN" };
inline const juce::Identifier chainID { "id" };
inline const juce::Identifier chainName { "name" };
inline const juce::Identifier chainMute { "mute" };
inline const juce::Identifier chainSolo { "solo" };
inline const juce::Identifier chainCrossover { "crossover" };
inline const juce::Identifier chainLowKey { "lowKey" };
inline const juce::Identifier chainHighKey { "highKey" };
inline const juce::Identifier chainLowVelocity { "lowVelocity" };
inline const juce::Identifier chainHighVelocity { "highVelocity" };

// The parameters of a container's chain, by the IDs their curves, lanes and modulations name them with, the chain's ID
// being their device's: its volume, as a fader's position from 0 to 1; its pan, from -1 to 1; and its crossover, in hertz.
// song
inline constexpr const char* chainVolumeParameter = "volume";
inline constexpr const char* chainPanParameter = "pan";
inline constexpr const char* crossoverParameter = "crossover";

// Child of a track's TRACK: a send to a return, in the order the track's sends were added (model/mixer/internal/Mixer.h).
// On SEND: its ID (DeviceID, of the song's); the return it sends to, by the return's track's ID (sendTo); its level in
// decibels, as a fader's (sendLevel; -100 for silence; none for 0 dB); and sendPreFader, true for a send before the
// track's fader, none for one after it. A send to a return the song hasn't any more stays, for an undo that brings the
// return back.
inline const juce::Identifier SEND { "SEND" };
inline const juce::Identifier sendID { "id" };
inline const juce::Identifier sendTo { "to" };
inline const juce::Identifier sendLevel { "level" };
inline const juce::Identifier sendPreFader { "preFader" };

//==============================================================================
// The song's modulation (model/modulation/internal/Modulation.h): each track's modulators and macros, and a container's
// macros, with what each modulates. song

// Child of TRACK and of MASTER: a modulator, in their order. On MODULATOR: its ID (ModulatorID, of the song's); its type,
// one of preset::modulatorTypeNames ("lfo"); and deviceOff, true when it is switched off. Its parameters' values set by
// hand are its VALUES, by their IDs in its description (devices/modulation/Modulators.h: "rate"), as a device's are, none
// for one never set; a step sequencer's steps are modulatorSteps: each step's value, -1 to 1, to a ten-thousandth,
// separated by spaces, without the zeros at the end.
inline const juce::Identifier MODULATOR { "MODULATOR" };
inline const juce::Identifier modulatorID { "id" };
inline const juce::Identifier modulatorType { "type" };
inline const juce::Identifier modulatorSteps { "steps" };

// Child of a track's TRACK and of a CONTAINER: a macro, in their order (a track's on tracks and returns, not on folders;
// a container has eight). On MACRO: its ID (ModulatorID, of the song's); its name; and its value, 0 to 1, none for 0.
inline const juce::Identifier MACRO { "MACRO" };
inline const juce::Identifier macroName { "name" };
inline const juce::Identifier macroValue { "value" };

// Child of MODULATOR and of MACRO: what it modulates, in the order its modulations were made. On MODULATES: the parameter,
// by its device's ID as a curve names it (curveDevice: "1042") and its own ID; and the amount, -1 to 1, to a
// ten-thousandth.
inline const juce::Identifier MODULATES { "MODULATES" };
inline const juce::Identifier modulatesDevice { "device" };
inline const juce::Identifier modulatesParameter { "parameter" };
inline const juce::Identifier modulatesAmount { "amount" };

//==============================================================================
// The song's automation (model/automation/internal/Automation.h): each automated parameter's curve, by its device and its
// own ID, with its points in beats. song

// Child of PHAZITE, added with the first curve: the curves (CURVE), in no particular order. A curve whose device the song
// hasn't any more stays while an undo could bring the device back, and goes as the project opens.
inline const juce::Identifier AUTOMATION { "AUTOMATION" };

// Child of AUTOMATION: a parameter's curve, with its points (POINT) in time order, two at one beat in the order they go (a
// jump). On CURVE: the device, by its ID (DeviceID: "1042"); and the parameter, by its ID in its device, as a lane names
// them (laneDevice, laneParameter).
inline const juce::Identifier CURVE { "CURVE" };
inline const juce::Identifier curveDevice { "device" };
inline const juce::Identifier curveParameter { "parameter" };

// Child of CURVE: a point. On POINT: where it is, in beats from the song's start (double, 0 or more); the parameter's value
// there (float, in the parameter's own range); and how the line to the next point bends (float -1..1, 0 for straight).
inline const juce::Identifier POINT { "POINT" };
inline const juce::Identifier pointBeat { "beat" };
inline const juce::Identifier pointValue { "value" };
inline const juce::Identifier pointCurve { "curve" };

// View settings: saved, written without undo steps, and not unsaved changes (decision 4; isUncountedProperty() and
// isUncountedElement(), below): on tracks and the master (whose row Collapse All collapses), their heights, collapsed and
// pinned, drum mode and rows, and their automation lanes; on PHAZITE, the playlist's grid; on devices (DEVICE), where their
// windows were. PIANOROLL and LANE hold view settings only. Keep these lists exact: anything else a project holds is song
// or state.
using IdentifierList = std::vector<const juce::Identifier*>;

inline bool isAmong (const juce::Identifier& p, const IdentifierList& list)
{
    return std::any_of (list.begin(), list.end(), [&p] (const juce::Identifier* name) { return *name == p; });
}

inline const IdentifierList& getTrackViewProperties()
{
    static const IdentifierList list { &trackHeight, &trackCollapsed, &trackPinned, &drumMode, &drumOrder, &drumHidden };
    return list;
}

inline const IdentifierList& getGridProperties()
{
    static const IdentifierList list { &gridSnap, &gridAdaptive, &gridOffset, &gridStep, &gridTriplets };
    return list;
}

// Written without the undo manager (model/mixer/internal/Mixer.h: rememberWindowPosition()).
inline const IdentifierList& getDeviceViewProperties()
{
    static const IdentifierList list { &deviceWindowX, &deviceWindowY };
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

inline bool isTrackViewElement (const juce::ValueTree& element)
{
    return element.hasType (LANE);
}

// What a project saves that is no change to it: view state (above); how the song plays rather than the song (design question
// 18): the loop (where it is, whether it is on, and whether it follows the song), the metronome's switch, where playback
// starts, the punch and the input chosen last; and the song's next ID (nextID), which the item it numbers comes with. As a
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
// The project's versions (model/versions/; docs/versions.md, and docs/versions-format.md for another tool): the song as it
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
    // merge gave it (to), and for a note the key of its clip ("CLIP:66"), so that a later merge of that line knows them.
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
    // version follows every other record's, so that commits another computer made on it since bring it back.
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
// The song's sections (model/arrangement/internal/Sections.h; docs/design.md, section 6): named, colored parts of the
// song over every track. song

// Child of PHAZITE, added with the first section: the sections (SECTION), in no particular order.
inline const juce::Identifier SECTIONS { "SECTIONS" };

// Child of SECTIONS: a section. Sections don't overlap.
inline const juce::Identifier SECTION { "SECTION" };

// On SECTION. int64, more than 0: its ID (SectionID), which the song gives it as it gives its tracks and clips theirs
// (Song::makeID()), so that no other item of the project has it.
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

// On SECTION. double 0..1, default 0.5: how intense it is, as the inspector shows it.
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
// keyRoot and keyScale. The first starts at 0; a lane without any is in C major.
inline const juce::Identifier CHORDKEY { "CHORDKEY" };

//==============================================================================
// The song's clips (model/arrangement/internal/Clips.h), each on its track's element: its MIDI clips with their notes,
// and its audio clips with the part of their files each plays and how. song

// Child of a TRACK that holds clips (trackIsTrack): a clip on it, in no particular order.
inline const juce::Identifier CLIP { "CLIP" };

// On CLIP. int64, more than 0: its ID (ClipID), which the song gives it (Song::makeID()).
inline const juce::Identifier clipID { "id" };

// On CLIP: what it holds: clipIsMidi, notes, or clipIsAudio, a part of an audio file.
inline const juce::Identifier clipKind { "kind" };
inline constexpr const char* clipIsMidi = "midi";
inline constexpr const char* clipIsAudio = "audio";

// On CLIP, or none: its name. A clip without one (a new MIDI clip, at first) is called after its track, whatever the track is
// called (model/arrangement/internal/Clips.h: getClipName()).
inline const juce::Identifier clipName { "name" };

// On CLIP. double, 0 or more: where it starts, in beats from the song's start. On a MIDI CLIP, double, more than 0: how long
// it lasts, in beats (an audio clip lasts as long as the part of its file it plays takes to play).
inline const juce::Identifier clipStart { "start" };
inline const juce::Identifier clipLength { "length" };

// On a MIDI CLIP. double, 0 or more, default 0: how far into its notes it starts, in beats. Its notes count from its content
// start, that many beats before its start: trimming its start or slipping it moves that.
inline const juce::Identifier clipOffset { "offset" };

// On CLIP. bool, default false: it is muted.
inline const juce::Identifier clipMute { "mute" };

// On CLIP. int64, or none: the alias group it is in, which has an ID of the song's (Song::makeID()). The clips of a group
// hold the same notes, with the same IDs: an edit to one clip's notes is made to each (model/notes/internal/Notes.h), as FL
// Studio's pattern clips play one pattern.
inline const juce::Identifier clipLink { "link" };

// Child of a MIDI CLIP: a note, in no particular order.
inline const juce::Identifier NOTE { "NOTE" };

// On NOTE. int64, 1 or more: its ID (NoteID, model/base/IDs.h), which no other note of its clip has, so that selections,
// undo and scripts can name one note (decided 2026-10-14). A copy of a clip, an alias too, keeps its notes' IDs. A new
// note's comes from the song's IDs (Song::makeID()), so that it is none a note had before.
inline const juce::Identifier noteID { "id" };

// On NOTE. int 0..127: its pitch, as a MIDI note number.
inline const juce::Identifier notePitch { "pitch" };

// On NOTE. double: where it starts, in beats from its clip's content start; and double, at least 1/64: how long it lasts,
// in beats.
inline const juce::Identifier noteStart { "start" };
inline const juce::Identifier noteLength { "length" };

// On NOTE. int 1..127: how hard it is played.
inline const juce::Identifier noteVelocity { "velocity" };

// On NOTE. bool, default false: it is muted.
inline const juce::Identifier noteMute { "mute" };

// On NOTE. bool, default false: it is a slide note, which plays nothing itself but glides the notes sounding as it starts to
// its pitch over its length; and a portamento note, which glides in from the note played before it (NoteValues.h).
inline const juce::Identifier noteSlide { "slide" };
inline const juce::Identifier notePortamento { "portamento" };

// Child of a MIDI CLIP: a controller event a recording took in with the notes, such as a sustain pedal or a bend, in MIDI's
// own terms: its kind, controlIsController (a MIDI controller: controlNumber 0..127),
// controlIsPitchWheel, controlIsChannelPressure, controlIsAftertouch (on the note controlNumber) or
// controlIsProgramChange; where it is, in beats from its clip's content start; and its value, 0..127, or 0..16383 for the
// pitch wheel.
inline const juce::Identifier CONTROL { "CONTROL" };
inline const juce::Identifier controlKind { "kind" };
inline constexpr const char* controlIsController = "controller";
inline constexpr const char* controlIsPitchWheel = "pitchWheel";
inline constexpr const char* controlIsChannelPressure = "channelPressure";
inline constexpr const char* controlIsAftertouch = "aftertouch";
inline constexpr const char* controlIsProgramChange = "programChange";
inline const juce::Identifier controlNumber { "number" };
inline const juce::Identifier controlBeat { "beat" };
inline const juce::Identifier controlValue { "value" };

//==============================================================================
// On an audio CLIP (model/arrangement/internal/AudioClips.h). One that follows the song's tempo, or is pitched or reversed,
// plays a render of its file that Phazite makes itself (roadmap step 2.18; model/arrangement/internal/ClipRenders.h).

// The audio file it plays: a path relative to the project's file, with / between its parts whatever the system, or a full
// path.
inline const juce::Identifier clipFile { "file" };

// double, 0 or more: the part of the file it plays, from and to, in seconds of the file as it was recorded, from < to,
// whichever way it plays.
inline const juce::Identifier clipFileStart { "fileStart" };
inline const juce::Identifier clipFileEnd { "fileEnd" };

// double 20..300 (AudioClipModel.h: minClipTempo..maxClipTempo), or none: the tempo the file was played at, in beats per
// minute, while the clip stretches to follow the song's; none while it plays at the file's own speed.
inline const juce::Identifier clipTempo { "tempo" };

// float -48..48, default 0: how many semitones it is pitched up or down, with cents as hundredths.
inline const juce::Identifier clipPitch { "pitch" };

// true, or none: it plays its file backwards.
inline const juce::Identifier clipReversed { "reversed" };

// float, default 0: how much louder or quieter it plays, in decibels.
inline const juce::Identifier clipGain { "gain" };

// double, 0 or more, default 0: how long it fades in, and out, in seconds; the shapes of the fades (PlaylistModel.h:
// FadeCurve), fadeIsCurve or fadeIsSCurve, default a curve; and how far each bends, a float from -1 to 1, default 0 (straight).
inline const juce::Identifier clipFadeIn { "fadeIn" };
inline const juce::Identifier clipFadeOut { "fadeOut" };
inline const juce::Identifier clipFadeInShape { "fadeInShape" };
inline const juce::Identifier clipFadeOutShape { "fadeOutShape" };
inline const juce::Identifier clipFadeInBend { "fadeInBend" };
inline const juce::Identifier clipFadeOutBend { "fadeOutBend" };
inline constexpr const char* fadeIsCurve = "curve";
inline constexpr const char* fadeIsSCurve = "sCurve";

// Child of an audio CLIP that follows the song's tempo, in no particular order: a warp marker (AudioClipModel.h), which pins a
// time of the file to where it plays, the audio between two of them stretched to fit, as Ableton Live's warp markers do. On
// WARP. double, 0 or more: its time in the file, in seconds as it was recorded; and double: where that time plays, as a time
// of the file played straight at the clip's tempo (clipTempo), in seconds, so that the markers keep their beats as the clip's
// tempo changes. song
inline const juce::Identifier WARP { "WARP" };
inline const juce::Identifier warpTime { "time" };
inline const juce::Identifier warpPlaysAt { "playsAt" };

//==============================================================================
// Recording audio (model/transport/RecordingModel.h; roadmap step 3.4).

// A track's input and how it is heard: trackInput and trackMonitoring, with the tracks above.

// On PHAZITE. The input chosen last on any track (an id as trackInput's), which the next track's list shows first. state
inline const juce::Identifier lastInput { "lastInput" };

// On PHAZITE. bool, default false: Record punches in and out at the looped range. Like the loop's switch, not an unsaved
// change. state
inline const juce::Identifier punch { "punch" };

// On an audio CLIP recorded round a looped range, whose file holds a take for each time round, one after another
// (ClipTakes). song
// int, 2 or more: how many takes the file holds.
inline const juce::Identifier clipTakes { "takes" };

// int, 1..clipTakes: the take the clip plays.
inline const juce::Identifier clipTake { "take" };

// double, more than 0: how long each take is, in seconds: the looped range's length when it was recorded.
inline const juce::Identifier clipTakeLength { "takeLength" };

// Phazite's drum sampler (devices/drums/DrumSampler.h) keeps in its DEVICE's SOUNDS what each pad's layers play, its pad
// named by its note (0 to 127): "pad36Sound" names one of its factory sounds for the first layer (devices/drums/
// FactoryDrums.h: "808 Kick"; with none, or a name that is none of theirs, the pad's own in the default kit, and nothing
// off the default kit's sixteen pads), and "pad36SoundFile" a sound file it plays instead while it is there: a path
// relative to the project's folder ("Audio/Kick 01.wav"), or in a preset its full path; "pad36Sound2" to "pad36Sound4" and
// "pad36SoundFile2" to "pad36SoundFile4" the same for its other layers, which play nothing without them.
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

inline const juce::Identifier& getPadSound (int pad, int layer = 0)
{
    static const auto names = makePadSoundNames ("Sound");
    return names[(size_t) (juce::jlimit (0, drumPads - 1, pad) * drumLayers + juce::jlimit (0, drumLayers - 1, layer))];
}

inline const juce::Identifier& getPadSoundFile (int pad, int layer = 0)
{
    static const auto names = makePadSoundNames ("SoundFile");
    return names[(size_t) (juce::jlimit (0, drumPads - 1, pad) * drumLayers + juce::jlimit (0, drumLayers - 1, layer))];
}

// Phazite's wavetable synth (devices/wavetable/WavetableSynth.h) keeps in its DEVICE's SOUNDS what each of its two
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

// Phazite's sampler (devices/sampler/Sampler.h) keeps in its DEVICE's SOUNDS what it plays: "sample" names one of its
// factory sounds (devices/sampler/FactorySamples.h: "808 Bass"; with none, or a name that is none of theirs, the first),
// "sampleFile" a sound file it plays instead while it is there: a path relative to the project's folder ("Audio/Vox.wav"),
// or in a preset its full path; "sampleSlices" the slices put in by hand, which stay whatever slices it, their starts as
// parts of the sound's length from 0 to 1, in order, a space between them ("0.125 0.25"); and "sampleSlicesRemoved" where
// slices it found were taken away by hand, the same way. None for none.
inline const juce::Identifier samplerSample { "sample" };
inline const juce::Identifier samplerSampleFile { "sampleFile" };
inline const juce::Identifier samplerSlices { "sampleSlices" };
inline const juce::Identifier samplerSlicesRemoved { "sampleSlicesRemoved" };

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
//                   samplerSlices, samplerSlicesRemoved): a file's full path, the factory sound's or table's name, or the
//                   slices } }
//     or a plug-in: { "plugin": { "format", "name", "manufacturer", "identifier", "uid" }, "name", "instrument",
//       "state": its own settings, in base 64, "program": its program, "mix": 0 to 1 }
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

    // What a modulator's "type" says, in ModulatorType's order (model/modulation/ModulationValues.h), macros left out.
    inline constexpr const char* modulatorTypeNames[] = { "lfo", "envelopeFollower", "steps", "random", "breakpoints", "noteFollower" };
} // namespace preset
} // namespace phazite::schema

namespace phazite
{
// The ID a song gives next: past its count (nextID) and every ID it holds.
std::uint64_t findNextID (const juce::ValueTree& song);

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
