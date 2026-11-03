// SPDX-License-Identifier: AGPL-3.0-or-later
// Containers (model/mixer/ContainerModel.h), for the model, the app and tests; not for the UI. A container is the song's
// (Schema.h: CONTAINER, with its name, mix, devices and its eight macros, MACRO), on a channel, playing its devices one
// after another, as one device. Containers don't nest, and hold no instrument. Tracktion mirrors one
// (model/mirror/internal/Mirror.h) as a rack instance playing a rack type of its own (Tracktion plays every instance of a
// type through one graph), which holds its devices in a row, wired one after the other (audio and MIDI).
//
// Rack types stay in the Edit for the whole session, also once no container plays them, as Tracktion's own rack list
// keeps them: Tracktion's plug-in cache keeps a container that was removed, to bring the same object back when the
// removal is undone, and that object plays the rack type object it was made with, while a rack type removed and added
// again by undo and redo would be another object. So rack types are added without undo steps and never removed in a
// session; an Edit built from the song as a project opens has only those its containers play.
#pragma once

#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/base/internal/Selection.h"
#include "model/base/internal/Tracktion.h"

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

//==============================================================================
// Tracktion's side, for the mirror and for presets, which make their devices in Tracktion until they are the song's
// (docs/tracktion-migration.md, T6 step 6).

// Whether one of Tracktion's devices is a container, and the rack type it plays (nullptr for another device); and the
// container that plays a rack type.
bool isContainer (const te::Plugin&);
te::RackType* getContainerRack (const te::Plugin&);
te::RackInstance* findContainerOf (const te::RackType&);

// A rack type's devices, in the order of their places in its state.
juce::Array<te::Plugin*> getRackDevices (const te::RackType&);

// A new container of devices made for it, as a preset's are (model/presets/internal/Presets.h), on no channel yet: its rack
// type with the devices in their order (not an instrument), wired, and its eight macros; named, or with a new container's
// name. Joins the caller's step.
te::Plugin::Ptr createContainer (te::Edit&, const te::Plugin::Array& devices, const juce::String& name);

// Deletes Tracktion's container, closing the windows of the devices it still holds (the mirror's, as the song's container
// goes).
void deleteContainer (te::RackInstance&);

// Adds a rack type to the Edit for the session (see above): a new one, empty but for its pins, or a copy of one (from
// a preset, or another container's, with new IDs). Not an undo step.
te::RackType::Ptr addContainerRack (te::Edit&);
te::RackType::Ptr addContainerRack (te::Edit&, const juce::ValueTree& rackState);

// Wires a container's devices one after the other, in their order, from its input to its output (straight through
// while it holds none), unless they are wired in their order already.
void wireContainer (te::RackType&);
} // namespace phazite
