// SPDX-License-Identifier: AGPL-3.0-or-later
// Modulation's Tracktion side (model/modulation/ModulationModel.h), for the model, the app and tests: a track's
// modulators, which are Tracktion's modifiers, and its macros, which are Tracktion's macro parameters, found by their
// ModulatorIDs; and what they modulate, which are Tracktion's modifier assignments. Not for the UI.
#pragma once

#include "model/base/internal/Tracktion.h"
#include "model/devices/ParameterRef.h"
#include "model/modulation/ModulationValues.h"

#include <optional>

namespace phazite
{
//==============================================================================
// A modulator or a macro: what moves the parameters assigned to it (Tracktion's ModifierSource), as one or the other.
struct ModulatorAccess
{
    te::Modifier::Ptr modifier;    // a modulator
    te::MacroParameter::Ptr macro; // or a macro

    te::AutomatableParameter::ModifierSource* getSource() const;
    te::Track* getTrack() const;            // whose it is: a container's macro's, its container's track
    te::RackType* getContainerRack() const; // a container's macro's: the rack type its container plays
    ModulatorID getID() const;
};

std::optional<ModulatorAccess> findModulator (const te::Edit&, ModulatorID);
ModulatorID getModulatorID (const te::Modifier&);
ModulatorID getModulatorID (const te::MacroParameter&);

// A track's modulators, in their order, then its macros: the master's and folders' too.
std::vector<ModulatorAccess> getModulators (te::Track&);

std::optional<ModulatorType> getModulatorType (const ModulatorAccess&);
juce::String getModulatorName (const ModulatorAccess&); // "LFO 2": its kind's name and its place among those of its track

// Adds one to the end of a track's modulators (a macro after its macros), joining the caller's step; nothing for a track
// that can't hold one (a macro: only tracks that hold clips can). It starts as Phazite sets it up: LFOs and random
// sources move both ways, and a step sequencer and an envelope follower at full depth, the steps rising.
std::optional<ModulatorAccess> addModulator (te::Track&, ModulatorType);

// Adds a macro to a list of them (a track's or a container's), named as given ("Macro 1"), at 0, so that what it
// modulates doesn't move until it is turned; made with its value in its state, so that redoing the step puts it back at
// 0 too. Joins the caller's step.
te::MacroParameter* addMacro (te::MacroParameterList&, const juce::String& name);

// Renames a macro, joining the caller's step; a name of nothing but spaces leaves it as it is.
void renameMacro (te::MacroParameter&, const juce::String& name);

// The name a modulator's editor shows for one of its parameters: Tracktion's, but clearer ("Polarity", "Point 1 Level").
juce::String getModulatorParameterName (const te::AutomatableParameter&);
void removeModulator (const ModulatorAccess&); // and what it modulates; joins the caller's step

// Whether its values go from -1 to 1, rather than from 0 to 1 (a macro's, an envelope follower's).
bool isBipolar (const ModulatorAccess&);

// Whether a modulator is on (its modifier's enabled parameter, which Tracktion's modulation reads); a macro always is.
// Switching one joins the caller's step, and what it modulates plays as it now is, modulated or not.
bool isModulatorOn (const ModulatorAccess&);
void setModulatorOn (const ModulatorAccess&, bool on);

//==============================================================================
// What a modulator or a macro modulates: a parameter, by an amount (Tracktion's assignment's value) from -1 to 1, as a
// part of the parameter's range.
struct ModulationAccess
{
    ModulatorAccess modulator;
    te::AutomatableParameter::Ptr parameter;
    te::AutomatableParameter::ModifierAssignment::Ptr assignment;
};

std::vector<ModulationAccess> getModulations (const te::AutomatableParameter&); // in the order they were made
std::vector<ModulationAccess> getModulations (const ModulatorAccess&);

// Whether a modulator can modulate a parameter: one of its own track's devices' (those in its containers too), or its
// track's volume, pan or sends, but none of its own (nor a macro's). A container's macros modulate its devices'.
bool canModulate (const ModulatorAccess&, const te::AutomatableParameter&);

// Makes a modulation or changes its amount, and takes it away, joining the caller's step. The parameter then plays its
// value as modulated where the playhead is, while the song doesn't play.
void setModulation (const ModulatorAccess&, te::AutomatableParameter&, float amount);
void removeModulation (const ModulatorAccess&, te::AutomatableParameter&);
} // namespace phazite
