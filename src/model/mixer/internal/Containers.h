// SPDX-License-Identifier: AGPL-3.0-or-later
// Containers (model/mixer/ContainerModel.h), for the model, the app and tests; not for the UI. A container is the song's
// (Schema.h: CONTAINER, with its name, mix and split, its chains, CHAIN, each with its devices, and its eight macros,
// MACRO), on a channel, playing as one device, as Phazite's engine plays it (model/playback/internal/SongPlayer.h). A
// container of effects plays its chains side by side, each on its part of the sound that comes in (ContainerValues.h:
// ContainerSplit), each through its own volume, pan, mute and solo, adds up what they put out and mixes it with what came
// in by its mix. An instrument container is a track's instrument: each chain plays its instrument on the notes in its key
// and velocity ranges, then its effects. Containers don't nest.
#pragma once

#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/base/internal/Selection.h"
#include "model/mixer/ContainerValues.h"

#include <vector>

namespace phazite
{
class CommandRegistry;

inline constexpr int macrosPerContainer = 8;

// Whether a device is a container, and an instrument container; its devices, all its chains' in their order; its macros, in
// their order; and the container a device is in, none for one on a channel.
bool isContainer (const Song&, DeviceID);
bool isInstrumentContainer (const Song&, DeviceID);
std::vector<DeviceID> getContainerDevices (const Song&, DeviceID container);
std::vector<ModulatorID> getContainerMacros (const Song&, DeviceID container);
DeviceID findContainerOf (const Song&, DeviceID device);

//==============================================================================
// Chains (Schema.h: CHAIN). A container's chains' elements, in their order; whether an element is a chain's; a chain's
// element by its ID, invalid for one the song hasn't; and the container's element a chain's, or a device's in a chain, is
// in (invalid for any other).
std::vector<juce::ValueTree> getChainStates (const juce::ValueTree& container);
bool isChainState (const juce::ValueTree&);
juce::ValueTree findContainerChain (const Song&, DeviceID chain);
juce::ValueTree getOwningContainer (const juce::ValueTree& chainOrDevice);

// By IDs: a container's chains, in their order; a chain's devices, in theirs; the chain a device in a container is in (none
// for one on a channel); the container a chain is in; and a chain's place among its container's.
std::vector<DeviceID> getChains (const Song&, DeviceID container);
std::vector<DeviceID> getChainDevices (const Song&, DeviceID chain);
DeviceID findChainOf (const Song&, DeviceID device);
DeviceID getChainContainer (const Song&, DeviceID chain);
int getChainIndex (const Song&, DeviceID chain);

// Whether a container can have another chain, and adding one after its chains, "Chain 2" and on, with nothing in it (on a
// container split into bands, its band starts between the last chain's crossover and the top: keepCrossoversInOrder()). A
// container holds maximumChains at most, and one split into its middle and side, or its left and right, two. Returns the
// chain, or none. Joins the caller's step.
bool canAddChain (const Song&, DeviceID container);
DeviceID addChain (Song&, DeviceID container);

// Removes a chain with its devices, unless it is its container's last; a chain left alone is heard again, neither muted
// nor soloed (it would be the container's switch, which is its own). Joins the caller's step.
bool canRemoveChain (const Song&, DeviceID chain);
void removeChain (Song&, DeviceID chain);

// A copy of a chain after it, with copies of its devices (new IDs, what plug-ins keep of their own stored first, its
// parameters' automation copied), named as it is; not on a container split into its middle and side, or its left and
// right, which has two. Returns it, or none. Joins the caller's step.
DeviceID duplicateChain (Song&, DeviceID chain);

// Moves a chain to another place among its container's (as moveDevice() moves a device), unless the container is split
// into bands, whose chains are in their bands' order. Joins the caller's step.
bool canMoveChains (const Song&, DeviceID container);
void moveChain (Song&, DeviceID chain, int newIndex);

// A chain's name (Schema.h: chainName), set in the caller's step; a name of nothing but spaces leaves it as it is.
juce::String getChainName (const Song&, DeviceID chain);
void setChainName (Song&, DeviceID chain, const juce::String&);

// A chain's mute and solo, as a track's (no undo steps): a chain soloed silences its container's others that aren't. And
// whether it is heard: neither muted nor silenced by another's solo.
bool isChainMuted (const Song&, DeviceID chain);
bool isChainSoloed (const Song&, DeviceID chain);
bool isChainHeard (const Song&, DeviceID chain);
void setChainMuted (Song&, DeviceID chain, bool);
void setChainSoloed (Song&, DeviceID chain, bool);

// A chain's volume, in decibels, and its pan, from -1 to 1, as a channel's (Schema.h: channelVolume, channelPan), set in the
// caller's step; its parameters' IDs are schema::chainVolumeParameter and chainPanParameter.
float getChainVolume (const Song&, DeviceID chain);
float getChainPan (const Song&, DeviceID chain);
void setChainVolume (Song&, DeviceID chain, float decibels);
void setChainPan (Song&, DeviceID chain, float pan);

//==============================================================================
// What part of the sound coming in each chain of a container of effects plays (ContainerValues.h), set in the caller's step:
// a split into the middle and side, or the left and right, needs two chains, and adds the second to a container of one
// (none for three or more: canSplit()); into bands, each chain after the first gets a crossover above the one before it.
// An instrument container's chains play notes, and split nothing.
ContainerSplit getContainerSplit (const Song&, DeviceID container);
bool canSplit (const Song&, DeviceID container, ContainerSplit);
void setContainerSplit (Song&, DeviceID container, ContainerSplit);

// Where a band starts, in hertz: a chain's crossover in a container split into bands (Schema.h: chainCrossover), the first
// chain's from the bottom (0). Its default, by its place among so many chains, spreads the bands as multiband effects do
// (120 Hz and 2.5 kHz for three). Setting one keeps it within 20 Hz and 20 kHz, in the caller's step. And giving each chain
// after the first a crossover above the one before it, where it hasn't one, as a container's chains change: the song's, in
// the caller's step, or an element made for it, through an undo manager or none.
inline constexpr float lowestCrossover = 20.0f, highestCrossover = 20000.0f;
float getDefaultCrossover (int chainIndex, int numChains);
float getChainCrossover (const Song&, DeviceID chain);
void setChainCrossover (Song&, DeviceID chain, float hertz);
void keepCrossoversInOrder (Song&, DeviceID container);
void keepCrossoversInOrder (juce::ValueTree container, juce::UndoManager*);

// The notes an instrument container's chain plays: its keys (0 to 127) and its velocities (1 to 127), all at first; set in
// the caller's step, each end kept within its range and the low at most the high.
NoteRange getChainKeys (const Song&, DeviceID chain);
NoteRange getChainVelocities (const Song&, DeviceID chain);
void setChainKeys (Song&, DeviceID chain, NoteRange);
void setChainVelocities (Song&, DeviceID chain, NoteRange);

//==============================================================================
// Whether devices can be grouped: devices of one channel (none in a container, nor a container): effects, into a container
// of effects; or a track's instrument with any of its effects, into an instrument container in the instrument's place.
// Grouping puts their elements into the new container's one chain, in their order on the channel, in the first one's
// place, keeping what they hold, their IDs and what modulates them, with eight macros, "Macro 1" to "Macro 8", each at 0,
// so that what one modulates doesn't move until it is turned. A device in a container listens to the sound passing through
// it, so a sidechain that listened to another track is turned off. Returns the container, or none. Joins the caller's step.
bool canGroupIntoContainer (const Song&, const std::vector<DeviceID>& devices);
DeviceID groupIntoContainer (Song&, const std::vector<DeviceID>& devices);

// An empty container of effects after a channel's effects, or the master's with an empty ID, made as grouping makes one,
// with one chain, for effects to be added to it (insertIntoChain()). Returns it, or none. Joins the caller's step.
DeviceID addContainer (Song&, TrackID channel);

// An instrument container as a track's instrument: the instrument the track had goes into its one chain, for others to be
// layered with it, or else the chain is empty. Returns it, or none for a track that can't hold an instrument. Joins the
// caller's step.
DeviceID addInstrumentContainer (Song&, TrackID);

// Puts the devices of a container of one chain back on its channel in its place, in their order (an instrument container's
// instrument the track's again), and removes the container with its macros and what they modulated; nothing for a container
// of several chains, whose sound that would change. Joins the caller's step.
bool canUngroupContainer (const Song&, DeviceID container);
void ungroupContainer (Song&, DeviceID container);

// Renames a container (Schema.h: containerName). Joins the caller's step.
void setContainerName (Song&, DeviceID container, const juce::String&);

// In a chain: adding a device's element, an effect at a place among its devices (-1 for the end); or an instrument, into an
// instrument container's chain, first, in place of the instrument it had. A container can't go in, nor an instrument into
// a container of effects. Moving one to another place in its chain (an instrument stays first), or into another chain of
// its container at a place among its devices (-1 for the end). Each joins the caller's step, and returns the device, or
// none. (Removing one is removeDevice() in Mixer.h, as for any device.)
bool canHoldInChain (const Song&, DeviceID chain, const juce::ValueTree& device);
DeviceID insertIntoChain (Song&, DeviceID chain, juce::ValueTree device, int index = -1);
void moveInChain (Song&, DeviceID device, int newIndex);
DeviceID moveToChain (Song&, DeviceID device, DeviceID chain, int index = -1);

// A chain's instrument, an instrument container's: its first device, if it is one; none for none.
DeviceID getChainInstrument (const Song&, DeviceID chain);

// A new container's element, in no channel yet: a new ID, its name (makeContainerName()), one chain with nothing in it, and
// its eight macros, "Macro 1" to "Macro 8", each at 0; an instrument container if asked. A new chain's element, with a new ID
// and a name, "Chain" and its place among so many. And a new container's name: "Container", or "Container 2" and on while
// the song has one of that name.
juce::ValueTree makeContainerState (Song&, bool instrument = false);
juce::ValueTree makeChainState (Song&, int number);
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
