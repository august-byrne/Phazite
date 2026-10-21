// SPDX-License-Identifier: AGPL-3.0-or-later
// Modulation as the UI reaches it (docs/design.md, section 10; roadmap step 2.3), by IDs and plain values
// (ModulationValues.h): each track's modulators (an LFO, an envelope follower, a step sequencer, a random source, a
// breakpoint envelope or a note follower) and macros, which modulate parameters of the track's devices and its volume,
// pan and sends, each by an amount. A parameter's modulation moves it around its value, or its automation's, as the song
// plays. Each function takes the open project's context, and gives nothing, or does nothing, for an ID of something that
// is gone. Edits are undo steps of their own, named as each says, unless a gesture is open (SongGesture.h): then they
// join its one step. The Tracktion side is internal/Modulation.h.
#pragma once

#include "ModulationValues.h"
#include "model/devices/ParameterHandle.h"

#include <memory>
#include <optional>
#include <vector>

namespace phazite
{
class ProjectContext;

//==============================================================================
// A track's modulators, in their order, then its macros; the master's too (AutomationModel.h: getMasterTrack()).
std::vector<ModulatorInfo> getModulators (const ProjectContext&, TrackID);
std::optional<ModulatorInfo> getModulator (const ProjectContext&, ModulatorID);

// Adds one after the track's others of its sort ("Add LFO"), one undo step; an empty ID for a track that holds none.
ModulatorID addModulator (ProjectContext&, TrackID, ModulatorType);

// Removes one and what it modulates ("Remove LFO 1"), one undo step.
void removeModulator (ProjectContext&, ModulatorID);

// Switches a modulator on or off ("Turn Modulator Off"), one undo step, so that a sound can be heard with and without it:
// off, it moves nothing, and what it modulates plays its value. It keeps what it modulates, and by how much. A macro has
// no switch, and is always on.
bool isModulatorOn (const ProjectContext&, ModulatorID);
void setModulatorOn (ProjectContext&, ModulatorID, bool on);

// A modulator's parameters, as its editor's controls change them (a macro's is its value): their IDs and names, in the
// modulator's order, and a handle of each, whose changes are undo steps named after them ("Change Rate").
std::vector<ModulatorParameter> getModulatorParameters (const ProjectContext&, ModulatorID);
ParameterHandle getModulatorParameter (ProjectContext&, ModulatorID, const juce::String& parameterID);

// Renames a macro, a track's or a container's ("Rename Macro"); a name of nothing but spaces leaves it as it is.
void renameMacro (ProjectContext&, ModulatorID, const juce::String& name);

// Its output over the last seconds, as the song played, oldest first, for its editor: from -1 to 1 for a bipolar one,
// else from 0 to 1. Tracktion keeps up to three seconds. And its output now, for its slot's meter (a macro's: its value,
// from 0 to 1).
std::vector<float> getModulatorOutput (const ProjectContext&, ModulatorID, double seconds);
float getModulatorValue (const ProjectContext&, ModulatorID);
bool isBipolar (const ProjectContext&, ModulatorID);

// A step sequencer's steps, each from -1 to 1, as many as it plays; and changing one, "Change Step" (in a gesture, a drag
// over several is one step).
std::vector<float> getSteps (const ProjectContext&, ModulatorID);
void setStep (ProjectContext&, ModulatorID, int index, float value);

//==============================================================================
// What modulators modulate (ModulationValues.h: Modulation).
std::vector<Modulation> getModulations (const ProjectContext&, const ParameterRef&); // a parameter's
std::vector<Modulation> getModulations (const ProjectContext&, ModulatorID);         // a modulator's

// Whether a modulator can modulate a parameter: one of its own track's devices', or its track's volume, pan and sends;
// not its own, nor a macro.
bool canModulate (const ProjectContext&, ModulatorID, const ParameterRef&);

// Modulates a parameter by an amount ("Modulate Cutoff"), or changes the amount of a modulation there is ("Change
// Modulation"); and takes one away ("Remove Modulation"). Each one undo step.
void modulate (ProjectContext&, ModulatorID, const ParameterRef&, float amount);
void removeModulation (ProjectContext&, ModulatorID, const ParameterRef&);

//==============================================================================
// The same for a control's parameter, as its menu, a modulator dropped on it and its ring reach it. Nothing for a handle
// of nothing, or of a parameter no modulator can modulate.

// The modulators that can modulate it: those of its track.
std::vector<ModulatorInfo> getModulatorsFor (const ParameterHandle&);
std::vector<Modulation> getModulations (const ParameterHandle&);

// A new modulator of a sort on its track, modulating it by an amount: one undo step, "Modulate Cutoff". Returns it.
ModulatorID modulateWithNew (const ParameterHandle&, ModulatorType, float amount);
void modulate (const ParameterHandle&, ModulatorID, float amount);
void removeModulation (const ParameterHandle&, ModulatorID);

// Changing a modulation's amount as the ring around a control is dragged: one undo step from the drag's start to its
// end, "Change Modulation". Nothing for a modulation there isn't.
class ModulationDrag
{
public:
    ~ModulationDrag(); // keeps what it changed

    float getStartAmount() const noexcept;
    void setAmount (float); // kept from -1 to 1

    struct Impl;
    explicit ModulationDrag (std::unique_ptr<Impl>);

private:
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE (ModulationDrag)
};

std::unique_ptr<ModulationDrag> beginModulationDrag (const ParameterHandle&, ModulatorID);

// What its modulations do to it now, for the ring: nothing while none modulates it.
std::optional<ModulationRange> getModulationRange (const ParameterHandle&);

// The amount a modulation starts with, a quarter of the parameter's range.
inline constexpr float defaultModulationAmount = 0.25f;
} // namespace phazite
