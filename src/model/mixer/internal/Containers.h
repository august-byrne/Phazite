// SPDX-License-Identifier: AGPL-3.0-or-later
// Containers' Tracktion side (model/mixer/ContainerModel.h), for the model, the app and tests; not for the UI. A
// container is the song's (Schema.h: CONTAINER, with its name, mix and devices; T1 step 5d), which Tracktion mirrors
// (model/mirror/internal/Mirror.h) as a rack instance on a channel, playing a rack type of its own (Tracktion plays every
// instance of a type through one graph), which holds its devices in a row, wired one after the other (audio and MIDI),
// and its eight macros (the song's too: Schema.h MACRO, T1 step 6).
//
// Rack types stay in the Edit for the whole session, also once no container plays them, as Tracktion's own rack list
// keeps them: Tracktion's plug-in cache keeps a container that was removed, to bring the same object back when the
// removal is undone, and that object plays the rack type object it was made with, while a rack type removed and added
// again by undo and redo would be another object. So rack types are added without undo steps and never removed in a
// session; an Edit built from the song as a project opens has only those its containers play.
#pragma once

#include "model/base/internal/Selection.h"
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

// A container's devices, in the order the sound goes through them (the song's, or its rack type's for one the song hasn't
// yet, a preset's); and its macros, in their order. And a rack type's devices, in the order of their places in its state.
juce::Array<te::Plugin*> getContainerDevices (const te::Plugin& container);
juce::ReferenceCountedArray<te::MacroParameter> getContainerMacros (const te::Plugin& container);
juce::Array<te::Plugin*> getRackDevices (const te::RackType&);

// The track a container is on, the master track for the master's; and the one a device in a container is on, through
// its container.
te::Track* getTrackOfContainer (const te::RackInstance&);
te::Track* getTrackOfContainerDevice (const te::Plugin&);

// Whether effects can be grouped: devices of one channel, none of them its instrument or a container. Grouping puts them
// into a new container in the first one's place, in their order on the channel, keeping their states, their IDs and
// what modulates them: their elements go into the container's in the song, and the mirror moves Tracktion's devices into
// its rack type. A device in a container listens to the sound passing through it, so a sidechain that listened to
// another track is turned off. Returns the container, or nullptr. Joins the caller's undo step.
bool canGroupIntoContainer (const juce::Array<te::Plugin*>&);
te::RackInstance* groupIntoContainer (const juce::Array<te::Plugin*>&);

// A new container of devices made for it, as a preset's are (model/presets/internal/Presets.h), on no channel yet: its rack
// type with the devices in their order (not an instrument), wired, and its eight macros; named, or with a new container's
// name. Put it on a channel as any device (Mixer.h: placeDevice(), insertDevice()). Joins the caller's step.
te::Plugin::Ptr createContainer (te::Edit&, const te::Plugin::Array& devices, const juce::String& name);

// Puts a container's devices back on its channel in its place, in their order, and removes the container with its
// macros and what they modulated. Joins the caller's step.
void ungroupContainer (te::RackInstance&);

// Renames a container, the song's (Schema.h: containerName), which its rack type mirrors. Joins the caller's step.
void setContainerName (te::Plugin& container, const juce::String&);

// Deletes Tracktion's container, closing the windows of the devices it still holds (the mirror's, as the song's container
// goes). Joins the caller's step.
void deleteContainer (te::RackInstance&);

// Adds a rack type to the Edit for the session (see above): a new one, empty but for its pins, or a copy of one (from
// a preset, or another container's, with new IDs). Not an undo step.
te::RackType::Ptr addContainerRack (te::Edit&);
te::RackType::Ptr addContainerRack (te::Edit&, const juce::ValueTree& rackState);

// Inside a container: adding a device (at a place among its devices, or -1 for the end: instruments can't go in), and
// moving one to another place, in the song, which the mirror follows, wiring the row again. Each joins the caller's step.
// (Removing one is removeDevice() in Mixer.h, as for any device.)
te::Plugin* insertIntoContainer (te::RackInstance&, const te::Plugin::Ptr&, int index = -1);
void moveInContainer (te::Plugin&, int newIndex);

// Wires a container's devices one after the other, in their order, from its input to its output (straight through
// while it holds none), unless they are wired in their order already. Joins the caller's step.
void wireContainer (te::RackType&);

// A new container's name: "Container", or "Container 2" and on while the song has one of that name (Schema.h:
// containerName).
juce::String makeContainerName (const te::Edit&);

// A container's macros as a new container gets them: eight, "Macro 1" to "Macro 8", each at 0, so that what one
// modulates doesn't move until it is turned (the song's containers get theirs in the song, from groupIntoContainer();
// this gives a preset's container, made on no channel yet, its own). Joins the caller's step.
void addContainerMacros (te::RackType&);

// After a track was copied (a folder with its tracks): the copy's containers play rack types of their own, copies of the
// original's, whose devices the copy's modulators modulate where the original's modulated the original's. Returns what
// each device (and macro) in the original's containers is in the copy's. Joins the caller's step.
te::EditItemID::IDMap copyContainers (const te::Track& original, te::Track& copy);

//==============================================================================
// The commands act on the selected devices: grouping them, which selects the container ("Group into Container"), and
// ungrouping the selected container, which selects its devices ("Ungroup Container").
bool canGroupSelectedDevices (const Selection&);
void groupSelectedDevices (Selection&);
bool canUngroupSelectedContainer (const Selection&);
void ungroupSelectedContainer (Selection&);

void addContainerCommands (CommandRegistry&, Selection&);
} // namespace phazite
