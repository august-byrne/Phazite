// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ContainerModel.h"
#include "internal/Containers.h"
#include "internal/Mixer.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/ParameterHandle.h"
#include "model/devices/internal/Parameters.h"
#include "model/modulation/ModulationModel.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

namespace phazite
{
namespace
{
    juce::String describeFrequency (float hertz)
    {
        return hertz < 1000.0f ? juce::String (juce::roundToInt (hertz)) + " Hz" : juce::String (hertz / 1000.0f, 2) + " kHz";
    }

    // What part of the sound a chain plays, as its container is split.
    juce::String describePart (const Song& song, DeviceID container, DeviceID chain)
    {
        if (isInstrumentContainer (song, container))
            return {};

        const auto chains = getChains (song, container);
        const auto index = (int) (std::find (chains.begin(), chains.end(), chain) - chains.begin());
        const auto count = (int) chains.size();

        switch (getContainerSplit (song, container))
        {
            case ContainerSplit::layers:
                return {};

            case ContainerSplit::midSide:
                return index == 0 ? "Mid" : index == 1 ? "Side" : "Nothing: two parts";

            case ContainerSplit::leftRight:
                return index == 0 ? "Left" : index == 1 ? "Right" : "Nothing: two parts";

            case ContainerSplit::bands:
            {
                if (count == 1)
                    return "All of it";

                // Each band from its crossover to the next one's, none below the one before it.
                auto below = 0.0f;
                std::vector<float> starts;

                for (int i = 0; i < count; ++i)
                {
                    below = i == 0 ? 0.0f : std::max (below, getChainCrossover (song, chains[(size_t) i]));
                    starts.push_back (below);
                }

                if (index == 0)
                    return "Below " + describeFrequency (starts[1]);

                if (index == count - 1)
                    return "Above " + describeFrequency (starts[(size_t) index]);

                return describeFrequency (starts[(size_t) index]) + " to " + describeFrequency (starts[(size_t) index + 1]);
            }
        }

        return {};
    }

    ChainInfo makeChainInfo (const ProjectContext& context, const Song& song, DeviceID chain)
    {
        ChainInfo info;
        info.id = chain;
        info.name = getChainName (song, chain);
        info.part = describePart (song, getChainContainer (song, chain), chain);
        info.muted = isChainMuted (song, chain);
        info.soloed = isChainSoloed (song, chain);
        info.heard = isChainHeard (song, chain);
        info.keys = getChainKeys (song, chain);
        info.velocities = getChainVelocities (song, chain);
        info.hasInstrument = getChainInstrument (song, chain).isValid();

        for (const auto device : getChainDevices (song, chain))
            info.devices.push_back (makeDeviceSlot (context, device));

        return info;
    }

    bool isChain (const Song& song, DeviceID chain)
    {
        return findContainerChain (song, chain).isValid();
    }
} // namespace

bool canGroupIntoContainer (const ProjectContext& context, const std::vector<DeviceID>& ids)
{
    auto* song = findSong (context);
    return song != nullptr && canGroupIntoContainer (*song, ids);
}

DeviceID groupIntoContainer (ProjectContext& context, const std::vector<DeviceID>& ids)
{
    auto* song = findSong (context);

    if (song == nullptr || ! canGroupIntoContainer (*song, ids))
        return {};

    const UndoStep step (*song, "Group into Container");
    return groupIntoContainer (*song, ids);
}

DeviceID addContainer (ProjectContext& context, TrackID channel)
{
    auto* song = findSong (context);

    if (song == nullptr || ! findChainState (*song, channel).isValid())
        return {};

    const UndoStep step (*song, "Add Container");
    return addContainer (*song, channel);
}

DeviceID addInstrumentContainer (ProjectContext& context, TrackID track)
{
    auto* song = findSong (context);

    if (song == nullptr || ! canHoldInstrument (context, track))
        return {};

    const UndoStep step (*song, "Add Instrument Container");
    return addInstrumentContainer (*song, track);
}

bool canUngroupContainer (const ProjectContext& context, DeviceID id)
{
    auto* song = findSong (context);
    return song != nullptr && isContainer (*song, id) && canUngroupContainer (*song, id);
}

void ungroupContainer (ProjectContext& context, DeviceID id)
{
    if (auto* song = findSong (context); song != nullptr && isContainer (*song, id) && canUngroupContainer (*song, id))
    {
        const UndoStep step (*song, "Ungroup Container");
        ungroupContainer (*song, id);
    }
}

std::vector<DeviceSlot> getContainerDevices (const ProjectContext& context, DeviceID id)
{
    std::vector<DeviceSlot> slots;

    if (auto* song = findSong (context))
        for (const auto device : getContainerDevices (*song, id))
            slots.push_back (makeDeviceSlot (context, device));

    return slots;
}

DeviceID getContainerOf (const ProjectContext& context, DeviceID id)
{
    auto* song = findSong (context);
    return song != nullptr ? findContainerOf (*song, id) : DeviceID();
}

bool isInstrumentContainer (const ProjectContext& context, DeviceID id)
{
    auto* song = findSong (context);
    return song != nullptr && isInstrumentContainer (*song, id);
}

void addEffectToContainer (ProjectContext& context, DeviceID id, const DeviceType& type)
{
    if (auto* song = findSong (context); song != nullptr && isContainer (*song, id))
        if (const auto chains = getChains (*song, id); ! chains.empty())
            addEffectToChain (context, chains.back(), type);
}

juce::String getContainerName (const ProjectContext& context, DeviceID id)
{
    auto* song = findSong (context);
    return song != nullptr && isContainer (*song, id) ? getDeviceName (*song, id) : juce::String();
}

void renameContainer (ProjectContext& context, DeviceID id, const juce::String& name)
{
    auto* song = findSong (context);
    const auto trimmed = name.trim();

    if (song == nullptr || ! isContainer (*song, id) || trimmed.isEmpty() || trimmed == getDeviceName (*song, id))
        return;

    const UndoStep step (*song, "Rename Container");
    setContainerName (*song, id, trimmed);
}

std::vector<ModulatorInfo> getContainerMacros (const ProjectContext& context, DeviceID id)
{
    std::vector<ModulatorInfo> macros;

    if (auto* song = findSong (context))
        for (const auto macro : getContainerMacros (*song, id))
            if (const auto info = getModulator (context, macro))
                macros.push_back (*info);

    return macros;
}

//==============================================================================
std::vector<ChainInfo> getChains (const ProjectContext& context, DeviceID container)
{
    std::vector<ChainInfo> chains;

    if (auto* song = findSong (context))
        for (const auto chain : getChains (*song, container))
            chains.push_back (makeChainInfo (context, *song, chain));

    return chains;
}

std::optional<ChainInfo> getChain (const ProjectContext& context, DeviceID chain)
{
    auto* song = findSong (context);
    return song != nullptr && isChain (*song, chain) ? std::optional (makeChainInfo (context, *song, chain)) : std::nullopt;
}

DeviceID getChainOf (const ProjectContext& context, DeviceID device)
{
    auto* song = findSong (context);
    return song != nullptr ? findChainOf (*song, device) : DeviceID();
}

DeviceID getContainerOfChain (const ProjectContext& context, DeviceID chain)
{
    auto* song = findSong (context);
    return song != nullptr ? getChainContainer (*song, chain) : DeviceID();
}

bool canAddChain (const ProjectContext& context, DeviceID container)
{
    auto* song = findSong (context);
    return song != nullptr && canAddChain (*song, container);
}

DeviceID addChain (ProjectContext& context, DeviceID container)
{
    auto* song = findSong (context);

    if (song == nullptr || ! canAddChain (*song, container))
        return {};

    const UndoStep step (*song, "Add Chain");
    return addChain (*song, container);
}

DeviceID duplicateChain (ProjectContext& context, DeviceID chain)
{
    auto* song = findSong (context);

    if (song == nullptr || ! isChain (*song, chain) || ! canAddChain (*song, getChainContainer (*song, chain)))
        return {};

    const UndoStep step (*song, "Duplicate Chain");
    return duplicateChain (*song, chain);
}

bool canRemoveChain (const ProjectContext& context, DeviceID chain)
{
    auto* song = findSong (context);
    return song != nullptr && canRemoveChain (*song, chain);
}

void removeChain (ProjectContext& context, DeviceID chain)
{
    if (auto* song = findSong (context); song != nullptr && canRemoveChain (*song, chain))
    {
        const UndoStep step (*song, "Remove Chain");
        removeChain (*song, chain);
    }
}

bool canMoveChains (const ProjectContext& context, DeviceID container)
{
    auto* song = findSong (context);
    return song != nullptr && canMoveChains (*song, container);
}

void moveChain (ProjectContext& context, DeviceID chain, int places)
{
    auto* song = findSong (context);

    if (song == nullptr || ! isChain (*song, chain) || ! canMoveChains (*song, getChainContainer (*song, chain)))
        return;

    const auto count = (int) getChains (*song, getChainContainer (*song, chain)).size();
    const auto target = getChainIndex (*song, chain) + places;

    if (places != 0 && juce::isPositiveAndBelow (target, count))
    {
        const UndoStep step (*song, "Move Chain");
        moveChain (*song, chain, target);
    }
}

void renameChain (ProjectContext& context, DeviceID chain, const juce::String& name)
{
    auto* song = findSong (context);
    const auto trimmed = name.trim();

    if (song == nullptr || ! isChain (*song, chain) || trimmed.isEmpty() || trimmed == getChainName (*song, chain))
        return;

    const UndoStep step (*song, "Rename Chain");
    setChainName (*song, chain, trimmed);
}

void toggleChainMute (ProjectContext& context, DeviceID chain)
{
    if (auto* song = findSong (context); song != nullptr && isChain (*song, chain))
        setChainMuted (*song, chain, ! isChainMuted (*song, chain));
}

void toggleChainSolo (ProjectContext& context, DeviceID chain)
{
    if (auto* song = findSong (context); song != nullptr && isChain (*song, chain))
        setChainSoloed (*song, chain, ! isChainSoloed (*song, chain));
}

void toggleChainSoloAlone (ProjectContext& context, DeviceID chain)
{
    auto* song = findSong (context);

    if (song == nullptr || ! isChain (*song, chain))
        return;

    // Soloed alone already: none soloed. Otherwise this one alone.
    const auto chains = getChains (*song, getChainContainer (*song, chain));
    const auto alone = isChainSoloed (*song, chain)
                       && std::none_of (chains.begin(), chains.end(),
                                        [&song, chain] (DeviceID other) { return other != chain && isChainSoloed (*song, other); });

    for (const auto other : chains)
        setChainSoloed (*song, other, ! alone && other == chain);
}

ParameterHandle getChainVolume (ProjectContext& context, DeviceID chain)
{
    auto* song = findSong (context);

    if (song == nullptr || ! isChain (*song, chain))
        return {};

    const ParameterRef ref { chain, schema::chainVolumeParameter };
    return makeParameterHandle (*song, ref, "Change " + getParameterName (*song, ref));
}

ParameterHandle getChainPan (ProjectContext& context, DeviceID chain)
{
    auto* song = findSong (context);

    if (song == nullptr || ! isChain (*song, chain))
        return {};

    const ParameterRef ref { chain, schema::chainPanParameter };
    return makeParameterHandle (*song, ref, "Change " + getParameterName (*song, ref));
}

ParameterHandle getChainCrossover (ProjectContext& context, DeviceID chain)
{
    auto* song = findSong (context);
    const ParameterRef ref { chain, schema::crossoverParameter };

    if (song == nullptr || ! locateParameter (*song, ref).has_value())
        return {};

    return makeParameterHandle (*song, ref, "Change " + getParameterName (*song, ref));
}

ContainerSplit getContainerSplit (const ProjectContext& context, DeviceID container)
{
    auto* song = findSong (context);
    return song != nullptr ? getContainerSplit (*song, container) : ContainerSplit::layers;
}

bool canSplit (const ProjectContext& context, DeviceID container, ContainerSplit split)
{
    auto* song = findSong (context);
    return song != nullptr && canSplit (*song, container, split);
}

void setContainerSplit (ProjectContext& context, DeviceID container, ContainerSplit split)
{
    auto* song = findSong (context);

    if (song == nullptr || ! canSplit (*song, container, split) || getContainerSplit (*song, container) == split)
        return;

    const UndoStep step (*song, "Split Container");
    setContainerSplit (*song, container, split);
}

juce::String getSplitName (ContainerSplit split)
{
    switch (split)
    {
        case ContainerSplit::layers:
            return "Layers";
        case ContainerSplit::bands:
            return "Frequency Bands";
        case ContainerSplit::midSide:
            return "Mid and Side";
        case ContainerSplit::leftRight:
            return "Left and Right";
    }

    return {};
}

void setChainKeys (ProjectContext& context, DeviceID chain, NoteRange keys)
{
    if (auto* song = findSong (context); song != nullptr && isChain (*song, chain) && getChainKeys (*song, chain) != keys)
    {
        const UndoStep step (*song, "Change Key Range");
        setChainKeys (*song, chain, keys);
    }
}

void setChainVelocities (ProjectContext& context, DeviceID chain, NoteRange velocities)
{
    if (auto* song = findSong (context); song != nullptr && isChain (*song, chain) && getChainVelocities (*song, chain) != velocities)
    {
        const UndoStep step (*song, "Change Velocity Range");
        setChainVelocities (*song, chain, velocities);
    }
}

void addEffectToChain (ProjectContext& context, DeviceID chain, const DeviceType& type)
{
    if (auto* song = findSong (context); song != nullptr && isChain (*song, chain) && ! type.isInstrument)
    {
        const UndoStep step (*song, "Add " + type.name);
        insertIntoChain (*song, chain, makeDeviceState (*song, type));
    }
}

void chooseChainInstrument (ProjectContext& context, DeviceID chain, const DeviceType& type)
{
    auto* song = findSong (context);

    if (song == nullptr || ! type.isInstrument || ! isChain (*song, chain)
        || ! isInstrumentContainer (*song, getChainContainer (*song, chain)))
        return;

    const UndoStep step (*song, (getChainInstrument (*song, chain).isValid() ? "Replace Instrument with " : "Add ") + type.name);
    insertIntoChain (*song, chain, makeDeviceState (*song, type));
}

void moveEffectToChain (ProjectContext& context, DeviceID device, DeviceID chain, int index)
{
    auto* song = findSong (context);

    if (song == nullptr || ! isChain (*song, chain) || findContainerOf (*song, device) != getChainContainer (*song, chain))
        return;

    // Nowhere new: nothing.
    const auto devices = getChainDevices (*song, chain);
    const auto at = (int) (std::find (devices.begin(), devices.end(), device) - devices.begin());

    if (findChainOf (*song, device) == chain && (index == at || index == at + 1 || (index < 0 && at == (int) devices.size() - 1)))
        return;

    const UndoStep step (*song, "Move Device");

    // In its own chain, to before the device at the index (after it when moving later, there being one fewer before it).
    if (findChainOf (*song, device) == chain)
        moveInChain (*song, device, index < 0 ? (int) devices.size() - 1 : index > at ? index - 1 : index);
    else
        moveToChain (*song, device, chain, index);
}
} // namespace phazite
