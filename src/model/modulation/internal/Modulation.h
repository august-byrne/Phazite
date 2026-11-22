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

#include "devices/modulation/Modulators.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/base/internal/ModulationValue.h"
#include "model/devices/ParameterRef.h"
#include "model/modulation/ModulationValues.h"

#include <cstdint>
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

// A modulator's or a macro's element in the song, invalid for one the song hasn't: for what still writes the element
// (model/devices/internal/Parameters.h: FoundParameter's state, which setValueSetByHand() writes), until the writers move
// onto the values (roadmap step S5.20, modulation's third piece). No reader asks it.
juce::ValueTree findModulatorState (const Song&, ModulatorID);

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

// Adds a macro to a track's or a container's element in the song (a TRACK that isn't a folder, or a CONTAINER), named as
// given, at 0, so that what it modulates doesn't move until it is turned. Returns it, or none for another element.
ModulatorID addMacro (Song&, juce::ValueTree owner, const juce::String& name);

// Renames a macro; a name of nothing but spaces leaves it as it is.
void renameMacro (Song&, ModulatorID, const juce::String& name);

// Removes a modulator or a macro, with what it modulates.
void removeModulator (Song&, ModulatorID);

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
// model/mixer/internal/Buses.h). In the caller's undo step.
void renameModulated (Song&, const ParameterRef& from, const ParameterRef& to);

// What modulates the originals of copies modulates the copies too, as much, by the IDs a copy was given (giveNewIDs():
// each original's to its copy's): for a copy that the modulators aren't in, a container's chain's, whose macros are the
// container's and whose track's modulators are the track's. In the caller's undo step.
void copyModulations (Song&, const std::map<std::uint64_t, std::uint64_t>& copied);

// The same for copies on another channel, which its own modulators must move: each of a channel's modulators and macros
// that modulates one of the originals is copied onto the other channel, set as it is, modulating the copies alone, as
// much. For a channel that plays in another's place with a fader and sends like its own (a bounce's track:
// model/mixer/internal/Freeze.h). In the caller's undo step.
void copyModulators (Song&, TrackID from, TrackID to, const std::map<std::uint64_t, std::uint64_t>& copied);
} // namespace phazite
