// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ContainerModel.h"
#include "internal/Containers.h"
#include "internal/Mixer.h"
#include "model/commands/internal/Undo.h"
#include "model/modulation/ModulationModel.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

namespace phazite
{
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

void ungroupContainer (ProjectContext& context, DeviceID id)
{
    if (auto* song = findSong (context); song != nullptr && isContainer (*song, id))
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
            slots.push_back (makeDeviceSlot (*song, device));

    return slots;
}

DeviceID getContainerOf (const ProjectContext& context, DeviceID id)
{
    auto* song = findSong (context);
    return song != nullptr ? findContainerOf (*song, id) : DeviceID();
}

void addEffectToContainer (ProjectContext& context, DeviceID id, const DeviceType& type)
{
    if (auto* song = findSong (context); song != nullptr && isContainer (*song, id) && ! type.isInstrument)
    {
        const UndoStep step (*song, "Add " + type.name);
        insertIntoContainer (*song, id, makeDeviceState (*song, type));
    }
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
} // namespace phazite
