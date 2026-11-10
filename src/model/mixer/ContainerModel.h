// SPDX-License-Identifier: AGPL-3.0-or-later
// Containers as the UI reaches them (docs/design.md, section 9; roadmap steps 2.4 and 4.11), by IDs and plain values:
// devices of a channel grouped into one device, which plays them in chains side by side and has eight macros, knobs that
// modulate the parameters of its devices and chains. A container of effects plays each chain on its part of the sound
// that comes in (all of it, a band, the middle or the side, the left or the right: ContainerValues.h), through the chain's
// own volume, pan, mute and solo, adds them up and mixes that with what came in by its mix. An instrument container is a
// track's instrument: each chain plays its instrument on the notes in its key and velocity ranges, then its effects.
// A container is a device, and so are the devices in it: the mixer's functions (MixerModel.h) show them, turn them on
// and off, move and remove them and give their mixes and key controls; and its macros are modulators
// (model/modulation/ModulationModel.h). Each function takes the open project's context, and gives nothing, or does
// nothing, for an ID of something that is gone. Changes are undo steps of their own, named as each says, but a chain's
// mute and solo, which are no undo steps, as a track's aren't. The model's side, by the song, is internal/Containers.h.
#pragma once

#include "ContainerValues.h"
#include "MixerModel.h"
#include "model/modulation/ModulationValues.h"

#include <optional>
#include <vector>

namespace phazite
{
// Whether devices can be grouped, and grouping them ("Group into Container"): effects of one channel into a container of
// effects in the first one's place; or a track's instrument with any of its effects into an instrument container in the
// instrument's place; none of them in a container, nor a container. Returns the container.
bool canGroupIntoContainer (const ProjectContext&, const std::vector<DeviceID>&);
DeviceID groupIntoContainer (ProjectContext&, const std::vector<DeviceID>&);

// An empty container of effects after a track's effects, or the master's with an empty ID ("Add Container"), with one
// chain, for effects to be added to (addEffectToChain()), as the effects' menu offers one. Returns it, or none for a track
// that isn't there.
DeviceID addContainer (ProjectContext&, TrackID channel);

// An instrument container as a track's instrument ("Add Instrument Container"), as the instruments' menu offers one: the
// instrument the track had goes into its one chain, for others to be layered with it. Returns it, or none for a track that
// can't hold an instrument, or whose instrument is an instrument container already (which can't go into another's chain).
bool canAddInstrumentContainer (const ProjectContext&, TrackID);
DeviceID addInstrumentContainer (ProjectContext&, TrackID);

// Whether a container can be ungrouped, one of one chain, and ungrouping it ("Ungroup Container"): its devices back on its
// channel in its place, without the container and its macros.
bool canUngroupContainer (const ProjectContext&, DeviceID);
void ungroupContainer (ProjectContext&, DeviceID);

// A container's devices, all its chains' in their order; the container a device is in, or an empty ID for one on a
// channel; and whether a container is an instrument container.
std::vector<DeviceSlot> getContainerDevices (const ProjectContext&, DeviceID container);
DeviceID getContainerOf (const ProjectContext&, DeviceID device);
bool isInstrumentContainer (const ProjectContext&, DeviceID);

// Adds an effect after a container's last chain's devices ("Add Reverb").
void addEffectToContainer (ProjectContext&, DeviceID container, const DeviceType&);

// A container's name, and renaming it ("Rename Container"); a name of nothing but spaces leaves it as it is.
juce::String getContainerName (const ProjectContext&, DeviceID);
void renameContainer (ProjectContext&, DeviceID, const juce::String&);

// A container's macros as modulators: "Macro 1" to "Macro 8" at first. Their knobs are their parameters
// (ModulationModel.h: getModulatorParameters()), and they modulate the parameters of the container's devices and its chains'
// volumes, pans and crossovers.
std::vector<ModulatorInfo> getContainerMacros (const ProjectContext&, DeviceID container);

//==============================================================================
// A container's chain as the UI shows it: its name; what part of the sound it plays, as its container is split ("Mid",
// "Side", "Left", "Right", "Below 250 Hz", "250 Hz to 2.50 kHz", "Above 2.50 kHz"; empty for a layer, and for an
// instrument container's chain); whether it is muted, soloed and heard (neither muted nor silenced by another chain's
// solo); the notes it plays (an instrument container's chain's); and its devices in their order, an instrument container's
// chain's instrument first, if it has one.
struct ChainInfo
{
    DeviceID id {};
    juce::String name {}, part {};
    bool muted = false, soloed = false, heard = true;
    NoteRange keys = allKeys, velocities = allVelocities;
    bool hasInstrument = false;
    std::vector<DeviceSlot> devices {};
};

// A container's chains, in their order; one of them; and the chain a device in a container is in (none for one on a
// channel).
std::vector<ChainInfo> getChains (const ProjectContext&, DeviceID container);
std::optional<ChainInfo> getChain (const ProjectContext&, DeviceID chain);
DeviceID getChainOf (const ProjectContext&, DeviceID device);
DeviceID getContainerOfChain (const ProjectContext&, DeviceID chain);

// Whether a container can have another chain (it holds maximumChains at most, and one split into its middle and side, or its
// left and right, two), and adding one after its chains, with nothing in it ("Add Chain"); a copy of one after it, with
// copies of its devices ("Duplicate Chain"). Each returns the chain, or none.
bool canAddChain (const ProjectContext&, DeviceID container);
DeviceID addChain (ProjectContext&, DeviceID container);
DeviceID duplicateChain (ProjectContext&, DeviceID chain);

// Whether a chain can be removed (not its container's last), and removing it with its devices ("Remove Chain").
bool canRemoveChain (const ProjectContext&, DeviceID chain);
void removeChain (ProjectContext&, DeviceID chain);

// Whether a container's chains move (not in a container split into bands, whose chains are in their bands' order), and
// moving one earlier (negative) or later among them ("Move Chain").
bool canMoveChains (const ProjectContext&, DeviceID container);
void moveChain (ProjectContext&, DeviceID chain, int places);

// Renaming a chain ("Rename Chain"); a name of nothing but spaces leaves it as it is.
void renameChain (ProjectContext&, DeviceID chain, const juce::String&);

// A chain's mute and solo: no undo steps, as a track's aren't. A chain soloed silences its container's others that aren't.
// Soloed alone, the others' solos go (or, soloed alone already, its own does), as a track's switch solos it.
void toggleChainMute (ProjectContext&, DeviceID chain);
void toggleChainSolo (ProjectContext&, DeviceID chain);
void toggleChainSoloAlone (ProjectContext&, DeviceID chain);

// A chain's volume and pan, as a channel's fader's, and its crossover, where its band starts in a container split into
// bands (a handle of nothing for the first chain, whose band starts at the bottom, and for a container not split into
// bands): parameters, which its container's macros, its track's modulators and automation move as they move a device's.
ParameterHandle getChainVolume (ProjectContext&, DeviceID chain);
ParameterHandle getChainPan (ProjectContext&, DeviceID chain);
ParameterHandle getChainCrossover (ProjectContext&, DeviceID chain);

// What part of the sound each chain of a container of effects plays, and splitting it so ("Split Container"): into the middle
// and side, or the left and right, two chains, the second added to a container of one (not for three or more); into bands,
// each chain after the first starting its band above the one before it. And what each split is called.
ContainerSplit getContainerSplit (const ProjectContext&, DeviceID container);
bool canSplit (const ProjectContext&, DeviceID container, ContainerSplit);
void setContainerSplit (ProjectContext&, DeviceID container, ContainerSplit);
juce::String getSplitName (ContainerSplit);

// The notes an instrument container's chain plays: its keys ("Change Key Range") and its velocities ("Change Velocity
// Range"), each end kept in its range and the low at most the high. A drag of them is one step: a SongGesture
// (model/commands/SongGesture.h) open while it moves them.
void setChainKeys (ProjectContext&, DeviceID chain, NoteRange);
void setChainVelocities (ProjectContext&, DeviceID chain, NoteRange);

// In a chain: an effect after its devices ("Add Reverb"); an instrument, an instrument container's chain's, in place of
// the one it had ("Add Synth", "Replace Instrument with Synth"); and an effect moved into another chain of its container,
// before the device at an index there (-1: after them), or in its own to there ("Move Device").
void addEffectToChain (ProjectContext&, DeviceID chain, const DeviceType&);
void chooseChainInstrument (ProjectContext&, DeviceID chain, const DeviceType&);
void moveEffectToChain (ProjectContext&, DeviceID device, DeviceID chain, int index);
} // namespace phazite
