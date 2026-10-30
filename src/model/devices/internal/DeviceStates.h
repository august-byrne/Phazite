// SPDX-License-Identifier: AGPL-3.0-or-later
// What devices hold of their state outside the project, stored in it outside the undo history.
//
// Part of a device's state stays in the device until something stores it in the project: a plug-in's own settings (its
// settings block and program, which its window changes) and channel layout, where its window was, and, for parameters
// that automation or modulation moves, the values set by hand; and, which Phazite adds, a plug-in's own IDs of the
// parameters the project names (PluginParameters.h). Tracktion stores it (flushPluginStateToValueTree() and
// what calls it) when it saves and before it copies a track or a rack, through the Edit's undo manager: every save was an
// undo step that changed nothing the user did, and undoing one that stored a plug-in's channel layout for the first time
// reset the playing plug-in. These store the same without the undo manager, and only what differs from what is there, as
// the undo manager would: Tracktion's own, called after them, finds it all stored and records nothing.
// Call them instead of Tracktion's, and before what of Tracktion's stores devices' states itself (Edit::copyTrack()).
// A plug-in's are stored in its element in the song too (Schema.h: DEVICE; docs/tracktion-migration.md, T1 step 5c),
// which the model also stores before it takes the plug-in away, so that an undo brings it back as it was.
#pragma once

#include "model/base/internal/Tracktion.h"

namespace phazite
{
// A device's state (ExternalPlugin's and Plugin's flushPluginStateToValueTree()). A container's devices are its rack's.
void storeDeviceState (te::Plugin&);

// A track's devices' states, with its clips' devices' (Track::flushStateToValueTree()) and, for a folder, those of the
// tracks in it, which Tracktion leaves as they are when it copies the folder.
void storeDeviceStates (te::Track&);

// A container's rack's devices' states (RackType::flushStateToValueTree()).
void storeDeviceStates (te::RackType&);

// Every device's and modulator's state in an Edit, with the version that saves it, by whom and its ID, as a save writes
// them (Edit::flushState()).
void storeDeviceStates (te::Edit&);

// Tracktion's forms of what the song keeps of a plug-in, for the mirror, which makes a plug-in from the song
// (model/mirror/internal/Mirror.h): its channel layout (ExternalPlugin's layout property) from the song's (Schema.h:
// pluginInputs, pluginOutputs), empty for none; and its values set by hand under automation (the parameters property of
// AutomatableEditItem) from the song's (VALUE), empty for none.
juce::MemoryBlock makeTracktionLayout (const juce::ValueTree& device);
juce::MemoryBlock makeTracktionValues (const juce::ValueTree& device);
} // namespace phazite
