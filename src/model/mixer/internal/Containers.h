// SPDX-License-Identifier: AGPL-3.0-or-later
// Containers (model/mixer/ContainerModel.h), for the model, the app and tests; not for the UI. A container is the song's
// (Schema.h: CONTAINER, with its name, mix, devices and its eight macros, MACRO), on a channel, playing its devices one
// after another, as one device, with its mix of their sound and the sound that comes into it, as Phazite's engine plays
// it (model/playback/internal/SongPlayer.h). Containers don't nest, and hold no instrument.
#pragma once

#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/base/internal/Selection.h"

#include <vector>

namespace phazite
{
class CommandRegistry;

inline constexpr int macrosPerContainer = 8;

// Whether a device is a container; its devices, in the order the sound goes through them; its macros, in their order; and
// the container a device is in, none for one on a channel.
bool isContainer (const Song&, DeviceID);
std::vector<DeviceID> getContainerDevices (const Song&, DeviceID container);
std::vector<ModulatorID> getContainerMacros (const Song&, DeviceID container);
DeviceID findContainerOf (const Song&, DeviceID device);

// Whether effects can be grouped: devices of one channel, none of them its instrument, in a container or a container.
// Grouping puts their elements into a new container's in the first one's place, in their order on the channel, keeping
// what they hold, their IDs and what modulates them, with eight macros, "Macro 1" to "Macro 8", each at 0, so that what
// one modulates doesn't move until it is turned. A device in a container listens to the sound passing through it, so a
// sidechain that listened to another track is turned off. Returns the container, or none. Joins the caller's undo step.
bool canGroupIntoContainer (const Song&, const std::vector<DeviceID>& effects);
DeviceID groupIntoContainer (Song&, const std::vector<DeviceID>& effects);

// Puts a container's devices back on its channel in its place, in their order, and removes the container with its
// macros and what they modulated. Joins the caller's step.
void ungroupContainer (Song&, DeviceID container);

// Renames a container (Schema.h: containerName). Joins the caller's step.
void setContainerName (Song&, DeviceID container, const juce::String&);

// Inside a container: adding a device's element (at a place among its devices, or -1 for the end: instruments and
// containers can't go in), and moving one to another place. Each joins the caller's step. (Removing one is removeDevice()
// in Mixer.h, as for any device.)
DeviceID insertIntoContainer (Song&, DeviceID container, juce::ValueTree device, int index = -1);
void moveInContainer (Song&, DeviceID device, int newIndex);

// A new container's name: "Container", or "Container 2" and on while the song has one of that name.
juce::String makeContainerName (const Song&);

//==============================================================================
// The commands act on the selected devices: grouping them, which selects the container ("Group into Container"), and
// ungrouping the selected container, which selects its devices ("Ungroup Container").
bool canGroupSelectedDevices (const Selection&);
void groupSelectedDevices (Selection&);
bool canUngroupSelectedContainer (const Selection&);
void ungroupSelectedContainer (Selection&);
void addContainerCommands (CommandRegistry&, Selection&);
} // namespace phazite
