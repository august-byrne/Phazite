// SPDX-License-Identifier: AGPL-3.0-or-later
// Modulation (model/modulation/ModulationModel.h), for the model, the app and tests: each track's modulators and macros,
// and each container's macros, which the song holds (Schema.h: MODULATOR, MACRO), found by their ModulatorIDs; and what
// each modulates (Schema.h: MODULATES), by the parameters' references (model/devices/internal/Parameters.h). Phazite's
// engine plays them from the song (model/playback/internal/SongPlayer.h). The changes join the caller's undo step. Not for
// the UI.
#pragma once

#include "devices/modulation/Modulators.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/devices/ParameterRef.h"
#include "model/modulation/ModulationValues.h"

#include <optional>
#include <vector>

namespace phazite
{
//==============================================================================
// The song's modulators and macros as it holds them: every one's element (the tracks', the master's and the containers');
// one's by its ID, invalid for one the song hasn't; and the ID an element names.
std::vector<juce::ValueTree> getModulatorStates (const Song&);
juce::ValueTree findModulatorState (const Song&, ModulatorID);
ModulatorID getModulatorID (const juce::ValueTree& modulatorState);

// What a MODULATES element names; a step sequencer's steps as its element keeps them; a modulator's type by the name the
// song gives it (Schema.h: modulatorType); and its kind as Phazite's engine plays it (devices/modulation/Modulators.h),
// whose parameters it has: nothing for a macro.
ParameterRef getModulatedParameter (const juce::ValueTree& modulates);
std::vector<float> getModulatorSteps (const juce::ValueTree& modulatorState);
std::optional<ModulatorType> findModulatorType (const juce::String& name);
std::optional<devices::ModulatorKind> getModulatorKind (const juce::ValueTree& modulatorState);

//==============================================================================
// A channel's modulators, in their order, then its macros: a track's, a folder's or the master's (by its ID, or none). A
// container's macros are its own (model/mixer/internal/Containers.h: getContainerMacros()).
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
} // namespace phazite
