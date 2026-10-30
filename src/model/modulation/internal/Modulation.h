// SPDX-License-Identifier: AGPL-3.0-or-later
// Modulation's Tracktion side (model/modulation/ModulationModel.h), for the model, the app and tests: a track's
// modulators, which are Tracktion's modifiers, and its macros, which are Tracktion's macro parameters, found by their
// ModulatorIDs; and what they modulate, which are Tracktion's modifier assignments. Each is the song's (Schema.h:
// MODULATOR, MACRO, MODULATES; T1 step 6), which the model changes, and Tracktion's mirror. Not for the UI.
#pragma once

#include "model/base/internal/Tracktion.h"
#include "model/devices/ParameterRef.h"
#include "model/modulation/ModulationValues.h"

#include <optional>

namespace phazite
{
class DevicePlugin;

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

// The song's modulation (Schema.h: MODULATOR, MACRO, MODULATES; T1 step 6), which Tracktion's modifiers, macro parameters
// and their assignments mirror (model/mirror/internal/Mirror.h: reconcileModulation()): every modulator's and macro's
// element (the tracks', the master's and the containers'); one's by its ID, invalid for one the song hasn't (a container's
// a preset made, before it is on a channel); and the ID an element names. The model changes them, and the mirror follows.
std::vector<juce::ValueTree> getModulatorStates (const te::Edit&);
juce::ValueTree findModulatorState (const te::Edit&, ModulatorID);
ModulatorID getModulatorID (const juce::ValueTree& modulatorState);

// What a MODULATES element names; a step sequencer's steps as its element keeps them; a modulator's type by the name the
// song gives it (Schema.h: modulatorType), and Tracktion's modifier that plays one of a type.
ParameterRef getModulatedParameter (const juce::ValueTree& modulates);
std::vector<float> getModulatorSteps (const juce::ValueTree& modulatorState);
std::optional<ModulatorType> findModulatorType (const juce::String& name);
const juce::Identifier& getModifierType (ModulatorType);

// After a track was copied (a folder with its tracks): what each of the original's modulators and macros is in the copy,
// the one in the same place, added to a map; and the elements of the modulators and macros, and of what they modulate, in
// the copy of its element given the copies' IDs, through a map that also holds its devices' (Mixer.h: mapCopiedDevices()).
void mapCopiedModulators (const te::Track& original, const te::Track& copy, te::EditItemID::IDMap&);
void giveCopiedModulatorIDs (juce::ValueTree& copiedTrack, const te::EditItemID::IDMap& copied);

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

// A step sequencer's step, -1 to 1 (the song's: Schema.h modulatorSteps), joining the caller's step.
void setModulatorStep (const ModulatorAccess&, int index, float value);

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

//==============================================================================
// Gives one of Phazite's devices, or each of them in an Edit, what modulates its parameters as Tracktion's assignments
// say (model/devices/internal/DeviceModulation.h), so that the device moves them itself in every stretch of a block
// (roadmap A5, part 4). A device whose modulation is as it was keeps playing it. Message thread.
void updateDeviceModulation (DevicePlugin&);
void updateDeviceModulation (te::Edit&);

// Keeps each of Phazite's devices in an Edit playing what modulates it: from the start, and after each turn of the
// message loop in which devices, modulators or macros came or went, or what they modulate changed. The project context
// keeps one for the open Edit.
class DeviceModulationKeeper : private juce::ValueTree::Listener,
                               private juce::AsyncUpdater
{
public:
    explicit DeviceModulationKeeper (te::Edit&);
    ~DeviceModulationKeeper() override;

private:
    te::Edit& edit;
    juce::ValueTree state;

    void changed (const juce::ValueTree& parent, const juce::ValueTree& child);
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override { changed (parent, child); }
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override { changed (parent, child); }
    void handleAsyncUpdate() override { updateDeviceModulation (edit); }

    JUCE_DECLARE_NON_COPYABLE (DeviceModulationKeeper)
};
} // namespace phazite
