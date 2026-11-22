// SPDX-License-Identifier: AGPL-3.0-or-later
// Modulation (model/modulation/ModulationModel.h), for the model, the app and tests: each track's modulators and macros,
// and each container's macros, which the song holds (Schema.h: MODULATOR, MACRO), found by their ModulatorIDs; and what
// each modulates (Schema.h: MODULATES), by the parameters' references (model/devices/internal/Parameters.h). Phazite's
// engine plays them from the song (model/playback/internal/SongPlayer.h). The changes join the caller's undo step. Not for
// the UI.
//
// Read as values (model/base/internal/ModulationValue.h: ModulatorValue, SongModulation; roadmap step S5.20, modulation's
// first piece): every reader of the song's modulation reads it through the readers below, which read the song's tree, still
// its storage, through the conversion. A reader that needs one modulator reads one (getModulatorValue()), one owner's its
// list (getOwnedModulators()), and what modulates one parameter asks findModulationsOf(), which reads no modulator whole:
// a knob asks it at each tick. The whole song's (getModulationValue()) is for what reads it all: the plan's compile, the
// passes.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/modulation/Modulators.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/base/internal/Immer.h"
#include "model/base/internal/ModulationValue.h"
#include "model/base/internal/Schema.h"
#include "model/devices/ParameterRef.h"
#include "model/modulation/ModulationValues.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace phazite
{
//==============================================================================
// The song's modulation, every modulator and macro of it: read from its tree (a walk of every channel and container), for
// what reads it all.
SongModulation getModulationValue (const Song&);

// A modulator or a macro by its ID, none for one the song hasn't (nor for a modulator of a type Phazite doesn't know, which
// the loader takes away): one element found and read, as much as finding its element cost.
std::optional<ModulatorValue> getModulatorValue (const Song&, ModulatorID);

// An owner's modulators and macros (ModulationValue.h: ModulatorPlace, the container's where it names one, else the
// channel's), in their order, modulators and macros together; none for an owner the song hasn't.
std::vector<ModulatorValue> getOwnedModulators (const Song&, const ModulatorPlace& owner);

// What modulates a parameter, whether or not the song has the parameter: each modulator's and macro's modulation of it, in
// the order of the song's modulators (its tracks', each channel's own then its containers', then the master's). No
// modulator read whole: what a knob asks at each tick (ModulationModel.h: getModulationRange()).
std::vector<Modulation> findModulationsOf (const Song&, const ParameterRef&);

// Whether one is a macro (none for one the song hasn't); a container's macros, in their order; and whether a track's
// modulators and macros, or its containers' macros, modulate anything.
bool isMacro (const Song&, ModulatorID);
std::vector<ModulatorID> getContainerMacros (const Song&, DeviceID container);
bool hasModulations (const Song&, TrackID);

// A modulator's type by the name the song gives it (Schema.h: modulatorType); and a value's type, its switch, whether it
// goes both ways (isBipolar(), below) and how many steps a step sequencer plays (1 for any other).
std::optional<ModulatorType> findModulatorType (const juce::String& name);
ModulatorType getModulatorType (const ModulatorValue&);
bool isModulatorOn (const ModulatorValue&);
bool isBipolar (const ModulatorValue&);
int getNumSteps (const ModulatorValue&);

// A modulator's or a macro's element in the song, invalid for one the song hasn't: for the parameters' code, whose
// FoundParameter holds it (model/devices/internal/Parameters.h), until modulation's switch (roadmap step S5.20). No reader
// reads it, and no writer writes it but the setters below.
juce::ValueTree findModulatorState (const Song&, ModulatorID);

//==============================================================================
// The song's modulation, one modulator or macro at a time, by its ID: what adds, changes, takes out or puts in order a
// modulator or a macro does it through these, each working its next value out from the one getModulatorValue() reads, by
// the rules of the edit, and setting it once; the rename of a modulation in place (renameModulated(), below), a control's
// set of one value (setModulatorValueSetByHand(), setMacroValue(), below, which write that value as these would) and the
// makers of a new container, which make it with its eight macros (model/mixer/internal/Containers.h: makeContainerState();
// model/presets: a container preset's), aside. Each joins the caller's undo step, as the song is written now: in the
// history's step in progress, or outside it while the song is written outside it (Song::OutsideHistory). While the song's
// tree is modulation's storage (roadmap step S5.20, until modulation's switch, which replaces their bodies alone), each
// writes the elements by what differs, so that every edit leaves the tree as Phazite's writers always left it. Whether it
// wrote: a value equal to the one held writes nothing, and makes no action in the history; nor does one whose place names an
// owner the song hasn't.
//   - setModulator(): a modulator or a macro made this value. One the song hasn't is made whole and added at the place given
//     in its owner's list (from 0; -1, as every edit adds one, after all the owner's elements), in one action. One it has
//     keeps its place, its element written where it differs from the value: each property by its name, as Phazite's writers
//     write it (a macro's value and a modulator's switch only while not 0 and on, its steps only while it has one); its
//     values set by hand as keepValueSetByHand() keeps them (Parameters.h: none at its default, the VALUES with the last);
//     what it modulates by the parameters: each it modulates no more taken out, each it keeps given its amount, each new
//     one added after the others (the only order an edit makes), then in the value's order (a version's). One the value
//     gives another owner (a version's) is moved to the place given in that owner's list first.
//   - eraseModulator(): one taken out, with what it modulates; whether it was there.
//   - setModulatorOrder(): an owner's modulators and macros of these IDs put first, in this order (an ID it hasn't left
//     out), the others after them in theirs, at the places its modulators and macros are at among its elements, its other
//     elements (its devices, lanes and sends) staying at theirs.
bool setModulator (Song&, const ModulatorValue&, int index = -1);
bool eraseModulator (Song&, ModulatorID);
bool setModulatorOrder (Song&, const ModulatorPlace& owner, const immer::flex_vector<ModulatorID>& order);

//==============================================================================
// A channel's modulators, in their order, then its macros: a track's, a folder's or the master's (by its ID, or none). A
// container's macros are its own (getContainerMacros()).
std::vector<ModulatorID> getModulators (const Song&, TrackID);

// What a modulator or a macro is; whose it is (a container's macro's: its container's channel); the container whose macro
// it is, none for others; and its name: a macro's own ("Macro 1"), or a modulator's kind and its place among those of its
// kind on its channel ("LFO 2").
std::optional<ModulatorType> getModulatorType (const Song&, ModulatorID);
TrackID getModulatorTrack (const Song&, ModulatorID);
DeviceID getModulatorContainer (const Song&, ModulatorID);
juce::String getModulatorName (const Song&, ModulatorID);

// Adds one to the end of a channel's modulators (a macro after its macros, named "Macro" and its number); none for a
// channel that can't hold it: modulators go on tracks, folders and the master, macros only on tracks and returns. It starts
// as Phazite sets it up: LFOs and random sources move both ways, and a step sequencer and an envelope follower at full
// depth, the steps rising.
ModulatorID addModulator (Song&, TrackID, ModulatorType);

// Adds a macro to the end of a track's (a track that isn't a folder: a track's or a return's), named as given, at 0, so
// that what it modulates doesn't move until it is turned. Returns it, or none for a track that can't hold one. A
// container's macros are made with it (model/mixer/internal/Containers.h).
ModulatorID addMacro (Song&, TrackID, const juce::String& name);

// Renames a macro; a name of nothing but spaces leaves it as it is.
void renameMacro (Song&, ModulatorID, const juce::String& name);

// Removes a modulator or a macro, with what it modulates, and what names it (model/mixer/internal/Mixer.h:
// forgetWhatNamesTheGone(): a MIDI link of a macro's value or of a modulator's parameter), in the caller's undo step.
void removeModulator (Song&, ModulatorID);

// A modulator's parameter's value set by hand (one of its kind's parameters: devices::getModulatorParameters()), none kept at
// its default; and a macro's value, 0 to 1. Nothing for one the song hasn't, or of the other kind, nor for the value it holds.
// As the parameters' code sets them (model/devices/internal/Parameters.h: setValueSetByHand(), which brings the value within
// its range first), at a control's drag's rate: so each finds the element once and reads and writes its one value, as
// setModulator() writes that part by what differs, reading none of it whole (tests/PerformanceGateTests.cpp).
void setModulatorValueSetByHand (Song&, ModulatorID, const devices::ParameterInfo&, float value);
void setMacroValue (Song&, ModulatorID, float value);

// Whether its values go from -1 to 1, rather than from 0 to 1: an LFO's, a random source's and a breakpoint envelope's as
// their polarity says, a step sequencer's always; not an envelope follower's, a note follower's or a macro's.
bool isBipolar (const Song&, ModulatorID);

// Whether a modulator is on (Schema.h: deviceOff); a macro always is. Off, it moves nothing, and what it modulates plays
// its value.
bool isModulatorOn (const Song&, ModulatorID);
void setModulatorOn (Song&, ModulatorID, bool on);

// A step sequencer's step, -1 to 1, of devices::Modulator::maxSteps (Schema.h: modulatorSteps).
void setModulatorStep (Song&, ModulatorID, int index, float value);

//==============================================================================
// What modulators and macros modulate (ModulationValues.h: Modulation): a parameter's, in the order of the song's
// modulators; and a modulator's, in the order its modulations were made. Only parameters the song has.
std::vector<Modulation> getModulations (const Song&, const ParameterRef&);
std::vector<Modulation> getModulations (const Song&, ModulatorID);

// Whether a modulator can modulate a parameter: one of its own channel's devices' (those in its containers too), or its
// channel's volume, pan or sends, but no modulator's and no macro's. A container's macros modulate its devices' alone.
bool canModulate (const Song&, ModulatorID, const ParameterRef&);

// Makes a modulation or changes its amount (-1 to 1, to a ten-thousandth), and takes it away.
void setModulation (Song&, ModulatorID, const ParameterRef&, float amount);
void removeModulation (Song&, ModulatorID, const ParameterRef&);

// Every modulation of a parameter made one of another, in its place, as much (a folder's fader that becomes the other kind:
// model/mixer/internal/Buses.h): the one edit that renames what a modulation names, in place (set anew, it would go after
// the others), its element's device and parameter written while the song's tree is modulation's storage. In the caller's
// undo step.
void renameModulated (Song&, const ParameterRef& from, const ParameterRef& to);

// Takes every modulation of a parameter of what is gone out of the song (gone: by the parameter's device's ID, a device's, a
// chain's, a fader's or a send's), each modulator's in one set: what names an item the song no longer holds
// (model/mixer/internal/Mixer.h: forgetWhatNamesTheGone(); model/versions/internal/ApplySong.h: removeBrokenReferences()). In
// the caller's undo step. How many it took out. Only the modulators that modulate what is gone are read (a device removed asks
// it: tests/PerformanceGateTests.cpp).
int forgetModulationsOf (Song&, const std::function<bool (DeviceID)>& gone);

// What modulates the originals of copies modulates the copies too, as much, by the IDs a copy was given (giveNewIDs():
// each original's to its copy's), after what it modulates, where it modulates none of the copy's parameter yet: for a copy
// that the modulators aren't in, a container's chain's, whose macros are the container's and whose track's modulators are
// the track's. In the caller's undo step.
void copyModulations (Song&, const std::map<std::uint64_t, std::uint64_t>& copied);

// The same for copies on another channel, which its own modulators must move: each of a channel's modulators and macros
// that modulates one of the originals is copied onto the other channel, set as it is, modulating the copies alone, as
// much. For a channel that plays in another's place with a fader and sends like its own (a bounce's track:
// model/mixer/internal/Freeze.h). Each copy's ID is added to copied, by its original's, before the copies' automation
// (Automation.h: copyCurves()), which names a macro's copy by it. In the caller's undo step.
void copyModulators (Song&, TrackID from, TrackID to, CopiedIDs& copied);

// After a part of the song was copied with new IDs (giveNewIDs(), Schema.h: a duplicated track): each copied owner's
// modulators and macros (a track's own, a container's) given to its copy, in their order, each with a new ID, which copied
// gets too, what they modulate of the copied items renamed to the copies, in the caller's undo step; nothing for a copy
// that holds some already (while the song's tree is modulation's storage, a copied element carries its modulators, as its
// IDs given anew name them). Before the copies' automation (Automation.h: copyCurves()), which names a macro's copy by the
// ID it gets here.
void copyModulation (Song&, CopiedIDs& copied);
} // namespace phazite
