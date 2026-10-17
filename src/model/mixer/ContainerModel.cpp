// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ContainerModel.h"
#include "internal/Containers.h"
#include "internal/Mixer.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/modulation/ModulationModel.h"
#include "model/modulation/internal/Modulation.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

namespace phazite
{
namespace
{
    te::Plugin* findDevice (const ProjectContext& context, DeviceID id)
    {
        auto* edit = getEdit (context);
        return edit != nullptr && id.isValid() ? te::findPluginForID (*edit, toTracktion (id)).get() : nullptr;
    }

    te::RackInstance* findContainer (const ProjectContext& context, DeviceID id)
    {
        return dynamic_cast<te::RackInstance*> (findDevice (context, id));
    }

    juce::Array<te::Plugin*> findDevices (const ProjectContext& context, const std::vector<DeviceID>& ids)
    {
        juce::Array<te::Plugin*> devices;

        for (const auto id : ids)
            devices.add (findDevice (context, id)); // nullptr for one that is gone, which can't be grouped

        return devices;
    }

    juce::Array<te::Plugin*> getSelectedDevices (const te::SelectionManager& selection)
    {
        juce::Array<te::Plugin*> devices;

        for (auto* device : selection.getItemsOfType<te::Plugin>())
            devices.add (device);

        return devices;
    }
} // namespace

//==============================================================================
bool canGroupIntoContainer (const ProjectContext& context, const std::vector<DeviceID>& ids)
{
    return canGroupIntoContainer (findDevices (context, ids));
}

DeviceID groupIntoContainer (ProjectContext& context, const std::vector<DeviceID>& ids)
{
    const auto devices = findDevices (context, ids);

    if (! canGroupIntoContainer (devices))
        return {};

    const UndoStep step (devices.getFirst()->edit, "Group into Container");
    auto* container = groupIntoContainer (devices);
    return container != nullptr ? getDeviceID (*container) : DeviceID();
}

void ungroupContainer (ProjectContext& context, DeviceID id)
{
    if (auto* container = findContainer (context, id))
    {
        const UndoStep step (container->edit, "Ungroup Container");
        ungroupContainer (*container);
    }
}

std::vector<DeviceSlot> getContainerDevices (const ProjectContext& context, DeviceID id)
{
    std::vector<DeviceSlot> slots;

    if (auto* container = findContainer (context, id))
        for (auto* device : getContainerDevices (*container))
            slots.push_back (makeDeviceSlot (*device));

    return slots;
}

DeviceID getContainerOf (const ProjectContext& context, DeviceID id)
{
    auto* device = findDevice (context, id);
    auto* container = device != nullptr ? findContainerOf (*device) : nullptr;
    return container != nullptr ? getDeviceID (*container) : DeviceID();
}

void addEffectToContainer (ProjectContext& context, DeviceID id, const DeviceType& type)
{
    if (auto* container = findContainer (context, id); container != nullptr && ! type.isInstrument)
    {
        const UndoStep step (container->edit, "Add " + type.name);
        insertIntoContainer (*container, createDevice (container->edit, type));
    }
}

juce::String getContainerName (const ProjectContext& context, DeviceID id)
{
    auto* container = findContainer (context, id);
    return container != nullptr ? container->getName() : juce::String();
}

void renameContainer (ProjectContext& context, DeviceID id, const juce::String& name)
{
    auto* container = findContainer (context, id);
    auto* rack = container != nullptr ? container->type.get() : nullptr;
    const auto trimmed = name.trim();

    if (rack == nullptr || trimmed.isEmpty() || trimmed == rack->rackName.get())
        return;

    const UndoStep step (rack->edit, "Rename Container");
    rack->rackName = trimmed;
}

std::vector<ModulatorInfo> getContainerMacros (const ProjectContext& context, DeviceID id)
{
    std::vector<ModulatorInfo> macros;

    if (auto* container = findContainer (context, id))
        for (auto* macro : getContainerMacros (*container))
            if (const auto info = getModulator (context, getModulatorID (*macro)))
                macros.push_back (*info);

    return macros;
}

//==============================================================================
bool canGroupSelectedDevices (const te::SelectionManager& selection)
{
    return canGroupIntoContainer (getSelectedDevices (selection));
}

void groupSelectedDevices (te::SelectionManager& selection)
{
    if (auto* container = groupIntoContainer (getSelectedDevices (selection)))
        selection.selectOnly (container);
}

bool canUngroupSelectedContainer (const te::SelectionManager& selection)
{
    const auto devices = getSelectedDevices (selection);
    return devices.size() == 1 && isContainer (*devices.getFirst());
}

void ungroupSelectedContainer (te::SelectionManager& selection)
{
    const auto devices = getSelectedDevices (selection);
    auto* container = devices.size() == 1 ? dynamic_cast<te::RackInstance*> (devices.getFirst()) : nullptr;

    if (container == nullptr)
        return;

    // Its devices, selected once they are back on its channel.
    te::Plugin::Array inside;

    for (auto* device : getContainerDevices (*container))
        inside.add (device);

    ungroupContainer (*container);
    selection.deselectAll();

    for (auto* device : inside)
        selection.addToSelection (device);
}

void addContainerCommands (CommandRegistry& registry, te::SelectionManager& selection)
{
    registry.add ({ .id = "device.group",
                    .name = "Group into Container",
                    .category = "Device",
                    .description = "Groups the selected effects into a container, which plays them one after the other and has "
                                   "eight macros that can turn several of their knobs at once.",
                    .synonyms = { "rack", "chain", "fx chain", "combine", "macro", "group devices" },
                    .perform = [&selection] { groupSelectedDevices (selection); },
                    .isEnabled = [&selection] { return canGroupSelectedDevices (selection); },
                    .undoTarget = undoTargetOf (selection),
                    .keyPriority = Command::notePriority }); // shares Ctrl+G with grouping tracks: devices are selected on their own

    registry.add ({ .id = "device.ungroup",
                    .name = "Ungroup Container",
                    .category = "Device",
                    .description = "Puts the selected container's effects back in its place, without the container and its macros.",
                    .synonyms = { "rack", "chain", "split", "unpack" },
                    .perform = [&selection] { ungroupSelectedContainer (selection); },
                    .isEnabled = [&selection] { return canUngroupSelectedContainer (selection); },
                    .undoTarget = undoTargetOf (selection),
                    .keyPriority = Command::notePriority });
}
} // namespace phazite
