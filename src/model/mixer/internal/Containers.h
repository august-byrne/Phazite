// SPDX-License-Identifier: AGPL-3.0-or-later
// Containers' Tracktion side (model/mixer/ContainerModel.h), for the model, the app and tests; not for the UI. A
// container is a Tracktion rack instance on a channel, playing a rack type of its own (Tracktion plays every instance
// of a type through one graph), which holds its devices in a row, wired one after the other (audio and MIDI), and its
// eight macros.
//
// Rack types stay in the Edit for the whole session, also once no container plays them, as Tracktion's own rack list
// keeps them: Tracktion's plug-in cache keeps a container that was removed, to bring the same object back when the
// removal is undone, and that object plays the rack type object it was made with, while a rack type removed and added
// again by undo and redo would be another object. So rack types are added without undo steps and never removed in a
// session; those no container plays are dropped when a project opens (dropUnusedContainerRacks()).
#pragma once

#include "model/base/internal/Tracktion.h"

namespace phazite
{
class CommandRegistry;

inline constexpr int macrosPerContainer = 8;

// Whether a device is a container, and the rack type it plays (nullptr for another device).
bool isContainer (const te::Plugin&);
te::RackType* getContainerRack (const te::Plugin&);

// The container a device is in, or nullptr for one on a channel; and the one that plays a rack type.
te::RackInstance* findContainerOf (const te::Plugin&);
te::RackInstance* findContainerOf (const te::RackType&);

// A container's devices, in the order the sound goes through them; and its macros, in their order.
juce::Array<te::Plugin*> getContainerDevices (const te::Plugin& container);
juce::ReferenceCountedArray<te::MacroParameter> getContainerMacros (const te::Plugin& container);

// The track a container is on, the master track for the master's; and the one a device in a container is on, through
// its container.
te::Track* getTrackOfContainer (const te::RackInstance&);
te::Track* getTrackOfContainerDevice (const te::Plugin&);

// Whether effects can be grouped: devices of one channel, none of them its instrument or a container. Grouping puts them
// into a new container in the first one's place, in their order on the channel, keeping their states, their IDs and
// what modulates them. A device in a container listens to the sound passing through it, so a sidechain that listened to
// another track is turned off. Returns the container, or nullptr. Joins the caller's undo step.
bool canGroupIntoContainer (const juce::Array<te::Plugin*>&);
te::RackInstance* groupIntoContainer (const juce::Array<te::Plugin*>&);

// Puts a container's devices back on its channel in its place, in their order, and removes the container with its
// macros and what they modulated. Joins the caller's step.
void ungroupContainer (te::RackInstance&);

// Deletes a container, closing its devices' windows. Joins the caller's step.
void deleteContainer (te::RackInstance&);

// Adds a rack type to the Edit for the session (see above): a new one, empty but for its pins, or a copy of one (from
// a preset, or another container's, with new IDs). Not an undo step.
te::RackType::Ptr addContainerRack (te::Edit&);
te::RackType::Ptr addContainerRack (te::Edit&, const juce::ValueTree& rackState);

// Drops the rack types no container plays from a saved project's state, before an Edit is made from it.
void dropUnusedContainerRacks (juce::ValueTree& editState);

// Inside a container: adding a device (at a place among its devices, or -1 for the end: instruments can't go in),
// moving one to another place, and deleting one. Each wires the row again, and joins the caller's step.
te::Plugin* insertIntoContainer (te::RackInstance&, const te::Plugin::Ptr&, int index = -1);
void moveInContainer (te::Plugin&, int newIndex);
void deleteFromContainer (te::Plugin&);

// Wires a container's devices one after the other, in their order, from its input to its output (straight through
// while it holds none). Joins the caller's step.
void wireContainer (te::RackType&);

// A new container's name: "Container", or "Container 2" and on while the song has one of that name.
juce::String makeContainerName (const te::Edit&);

// A container's macros as a new container gets them: eight, "Macro 1" to "Macro 8", each at 0, so that what one
// modulates doesn't move until it is turned. Joins the caller's step.
void addContainerMacros (te::RackType&);

// After a track was copied (a folder with its tracks): the copy's containers play rack types of their own, copies of the
// original's, whose devices the copy's modulators modulate where the original's modulated the original's. Returns what
// each device (and macro) in the original's containers is in the copy's. Joins the caller's step.
te::EditItemID::IDMap copyContainers (const te::Track& original, te::Track& copy);

//==============================================================================
// The commands act on the selected devices: grouping them, which selects the container ("Group into Container"), and
// ungrouping the selected container, which selects its devices ("Ungroup Container").
bool canGroupSelectedDevices (const te::SelectionManager&);
void groupSelectedDevices (te::SelectionManager&);
bool canUngroupSelectedContainer (const te::SelectionManager&);
void ungroupSelectedContainer (te::SelectionManager&);

void addContainerCommands (CommandRegistry&, te::SelectionManager&);
} // namespace phazite
