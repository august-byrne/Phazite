// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Containers.h"
#include "internal/Mixer.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"

#include <algorithm>

namespace phazite
{
bool isContainer (const Song& song, DeviceID id)
{
    return isContainerState (findDeviceState (song, id));
}

std::vector<DeviceID> getContainerDevices (const Song& song, DeviceID container)
{
    std::vector<DeviceID> ids;

    if (const auto state = findDeviceState (song, container); isContainerState (state))
        for (const auto& inside : getDeviceStates (state))
            ids.push_back (getDeviceID (inside));

    return ids;
}

std::vector<ModulatorID> getContainerMacros (const Song& song, DeviceID container)
{
    std::vector<ModulatorID> ids;

    if (const auto state = findDeviceState (song, container); isContainerState (state))
        for (const auto& child : state)
            if (child.hasType (schema::MACRO))
                ids.push_back (ModulatorID::fromRaw ((std::uint64_t) (juce::int64) child[schema::modulatorID]));

    return ids;
}

DeviceID findContainerOf (const Song& song, DeviceID device)
{
    const auto parent = findDeviceState (song, device).getParent();
    return isContainerState (parent) ? getDeviceID (parent) : DeviceID();
}

//==============================================================================
bool canGroupIntoContainer (const Song& song, const std::vector<DeviceID>& effects)
{
    if (effects.empty())
        return false;

    // Effects on one channel: none in a container already, nor a container itself.
    const auto chain = findDeviceState (song, effects.front()).getParent();

    return chain.isValid() && ! isContainerState (chain)
           && std::all_of (effects.begin(), effects.end(),
                           [&song, &chain] (DeviceID id)
                           {
                               const auto state = findDeviceState (song, id);
                               return state.getParent() == chain && state.hasType (schema::DEVICE) && ! isInstrumentState (state);
                           });
}

DeviceID groupIntoContainer (Song& song, const std::vector<DeviceID>& effects)
{
    if (! canGroupIntoContainer (song, effects))
        return {};

    // In their order on the channel, in the first one's place.
    auto chain = findDeviceState (song, effects.front()).getParent();
    std::vector<juce::ValueTree> grouped;

    for (const auto& state : getDeviceStates (chain))
        if (std::find (effects.begin(), effects.end(), getDeviceID (state)) != effects.end())
            grouped.push_back (state);

    // The song's container, with a new ID and name, and its eight macros; the devices' elements go into it, with what they
    // hold of their states (which an undo brings back).
    auto* undo = &song.getUndoManager();
    song.storeDeviceStates (getTrackID (chain).getRaw());
    const auto id = DeviceID::fromRaw (song.makeID());
    juce::ValueTree container (schema::CONTAINER);
    container.setProperty (schema::deviceID, (juce::int64) id.getRaw(), nullptr);
    container.setProperty (schema::containerName, makeContainerName (song), nullptr);

    for (int number = 1; number <= macrosPerContainer; ++number)
    {
        juce::ValueTree macro (schema::MACRO);
        macro.setProperty (schema::modulatorID, (juce::int64) song.makeID(), nullptr);
        macro.setProperty (schema::macroName, "Macro " + juce::String (number), nullptr);
        container.appendChild (macro, nullptr);
    }

    chain.addChild (container, chain.indexOf (grouped.front()), undo);

    for (auto state : grouped)
    {
        state.removeProperty (schema::deviceSidechain, undo);
        chain.removeChild (state, undo);
        container.appendChild (state, undo);
    }

    return id;
}

void ungroupContainer (Song& song, DeviceID id)
{
    auto state = findDeviceState (song, id);

    if (! isContainerState (state))
        return;

    // Its devices' elements go back in its place, in their order, and it goes with its macros and what they modulated.
    auto* undo = &song.getUndoManager();
    auto chain = state.getParent();
    auto index = chain.indexOf (state);

    for (auto inside : getDeviceStates (state))
    {
        state.removeChild (inside, undo);
        chain.addChild (inside, index++, undo);
    }

    chain.removeChild (state, undo);
}

void setContainerName (Song& song, DeviceID id, const juce::String& name)
{
    if (auto state = findDeviceState (song, id); isContainerState (state))
        state.setProperty (schema::containerName, name, &song.getUndoManager());
}

DeviceID insertIntoContainer (Song& song, DeviceID container, juce::ValueTree device, int index)
{
    auto state = findDeviceState (song, container);

    if (! isContainerState (state) || ! device.hasType (schema::DEVICE) || isInstrumentState (device))
        return {};

    const auto devices = getDeviceStates (state);
    const auto at = juce::isPositiveAndBelow (index, (int) devices.size()) ? state.indexOf (devices[(size_t) index]) : -1;
    state.addChild (device, at, &song.getUndoManager());
    return getDeviceID (device);
}

void moveInContainer (Song& song, DeviceID device, int newIndex)
{
    auto state = findDeviceState (song, device);
    auto container = state.getParent();

    if (! isContainerState (container))
        return;

    // To the place of the device that is there now: after it when moving later, before it when moving earlier.
    const auto devices = getDeviceStates (container);
    const auto& target = devices[(size_t) juce::jlimit (0, (int) devices.size() - 1, newIndex)];
    container.moveChild (container.indexOf (state), container.indexOf (target), &song.getUndoManager());
}

juce::String makeContainerName (const Song& song)
{
    juce::StringArray taken;
    auto chains = getTrackStates (song);
    chains.push_back (findChainState (song, {}));

    for (const auto& chain : chains)
        for (const auto& state : getDeviceStates (chain))
            if (isContainerState (state))
                taken.add (state[schema::containerName].toString());

    juce::String name ("Container");

    for (int number = 2; taken.contains (name); ++number)
        name = "Container " + juce::String (number);

    return name;
}

//==============================================================================
bool canGroupSelectedDevices (const Selection& selection)
{
    auto* song = selection.getSong();
    return song != nullptr && canGroupIntoContainer (*song, selection.getDevices());
}

void groupSelectedDevices (Selection& selection)
{
    if (auto* song = selection.getSong())
        if (const auto container = groupIntoContainer (*song, selection.getDevices()); container.isValid())
            selection.selectOnly (container);
}

bool canUngroupSelectedContainer (const Selection& selection)
{
    auto* song = selection.getSong();
    const auto devices = selection.getDevices();
    return song != nullptr && devices.size() == 1 && isContainer (*song, devices.front());
}

void ungroupSelectedContainer (Selection& selection)
{
    if (! canUngroupSelectedContainer (selection))
        return;

    // Its devices, selected once they are back on its channel.
    auto& song = *selection.getSong();
    const auto container = selection.getDevices().front();
    const auto inside = getContainerDevices (song, container);
    ungroupContainer (song, container);
    selection.deselectAll();

    for (const auto device : inside)
        selection.addToSelection (device);
}

void addContainerCommands (CommandRegistry& registry, Selection& selection)
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
