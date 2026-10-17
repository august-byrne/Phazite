// SPDX-License-Identifier: AGPL-3.0-or-later
// Containers as the UI reaches them (docs/design.md, section 9; roadmap step 2.4), by IDs and plain values: effects of
// a channel grouped into one device, which plays them one after the other and has eight macros, knobs that modulate
// the parameters of its devices. A container is a device, and so are the devices in it: the mixer's functions
// (MixerModel.h) show them, turn them on and off, move and remove them and give their mixes and key controls (a
// container's are its first macros); and its macros are modulators (model/modulation/ModulationModel.h). Each function
// takes the open project's context, and gives nothing, or does nothing, for an ID of something that is gone. Changes
// are undo steps of their own, named as each says. The Tracktion side is internal/Containers.h.
#pragma once

#include "MixerModel.h"
#include "model/modulation/ModulationValues.h"

#include <vector>

namespace phazite
{
// Whether effects can be grouped, and grouping them ("Group into Container"): effects of one channel, none of them its
// instrument or a container, into a container in the first one's place, in their order on the channel. Returns the
// container.
bool canGroupIntoContainer (const ProjectContext&, const std::vector<DeviceID>&);
DeviceID groupIntoContainer (ProjectContext&, const std::vector<DeviceID>&);

// Puts a container's devices back on its channel in its place, without the container and its macros ("Ungroup
// Container").
void ungroupContainer (ProjectContext&, DeviceID);

// A container's devices, in the order the sound goes through them; and the container a device is in, or an empty ID
// for one on a channel.
std::vector<DeviceSlot> getContainerDevices (const ProjectContext&, DeviceID container);
DeviceID getContainerOf (const ProjectContext&, DeviceID device);

// Adds an effect after a container's devices ("Add Reverb").
void addEffectToContainer (ProjectContext&, DeviceID container, const DeviceType&);

// A container's name, and renaming it ("Rename Container"); a name of nothing but spaces leaves it as it is.
juce::String getContainerName (const ProjectContext&, DeviceID);
void renameContainer (ProjectContext&, DeviceID, const juce::String&);

// A container's macros as modulators: "Macro 1" to "Macro 8" at first. Their knobs are their parameters
// (ModulationModel.h: getModulatorParameters()), and they modulate the parameters of the container's devices.
std::vector<ModulatorInfo> getContainerMacros (const ProjectContext&, DeviceID container);
} // namespace phazite
