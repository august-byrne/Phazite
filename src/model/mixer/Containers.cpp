// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Containers.h"
#include "internal/Mixer.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Devices.h"

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

    song.reconcile (Song::Part::devices);
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
    song.reconcile (Song::Part::devices);
}

void setContainerName (Song& song, DeviceID id, const juce::String& name)
{
    if (auto state = findDeviceState (song, id); isContainerState (state))
    {
        state.setProperty (schema::containerName, name, &song.getUndoManager());
        song.reconcile (Song::Part::devices);
    }
}

DeviceID insertIntoContainer (Song& song, DeviceID container, juce::ValueTree device, int index)
{
    auto state = findDeviceState (song, container);

    if (! isContainerState (state) || ! device.hasType (schema::DEVICE) || isInstrumentState (device))
        return {};

    const auto devices = getDeviceStates (state);
    const auto at = juce::isPositiveAndBelow (index, (int) devices.size()) ? state.indexOf (devices[(size_t) index]) : -1;
    state.addChild (device, at, &song.getUndoManager());
    song.reconcile (Song::Part::devices);
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
    song.reconcile (Song::Part::devices);
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

//==============================================================================
bool isContainer (const te::Plugin& device)
{
    return dynamic_cast<const te::RackInstance*> (&device) != nullptr;
}

te::RackType* getContainerRack (const te::Plugin& device)
{
    auto* container = dynamic_cast<const te::RackInstance*> (&device);
    return container != nullptr ? container->type.get() : nullptr;
}

te::RackInstance* findContainerOf (const te::RackType& rack)
{
    return te::getRackInstancesInEditForType (rack).getFirst(); // each container plays a rack type of its own
}

juce::Array<te::Plugin*> getRackDevices (const te::RackType& rack)
{
    juce::Array<te::Plugin*> devices;
    const auto plugins = rack.getPlugins();

    for (const auto& place : rack.state)
        if (place.hasType (te::IDs::PLUGININSTANCE))
            for (auto* plugin : plugins)
                if (plugin->state.getParent() == place)
                    devices.add (plugin);

    return devices;
}

namespace
{
    // A container's macros as a new container gets them (groupIntoContainer() says how), here for a preset's container,
    // made on no channel yet, with their values in their states, so that redoing their container's step puts them back at
    // 0 too.
    void addContainerMacros (te::RackType& rack)
    {
        auto& macros = rack.getMacroParameterListForWriting();
        auto& edit = rack.edit;

        for (auto number = macros.getMacroParameters().size() + 1; number <= macrosPerContainer; ++number)
        {
            juce::ValueTree macro (te::IDs::MACROPARAMETER);
            te::EditItemID::fromRawID (getSong (edit).makeID()).writeID (macro, nullptr); // the song's once it takes the container in
            macro.setProperty (te::IDs::name, "Macro " + juce::String (number), nullptr);
            macro.setProperty (te::IDs::value, 0.0f, nullptr);
            macros.state.addChild (macro, -1, &edit.getUndoManager());
        }
    }
} // namespace

te::Plugin::Ptr createContainer (te::Edit& edit, const te::Plugin::Array& devices, const juce::String& name)
{
    const auto rack = addContainerRack (edit);

    if (rack == nullptr)
        return {};

    rack->rackName = name.isNotEmpty() ? name : makeContainerName (getSong (edit));

    for (const auto& device : devices)
        if (device != nullptr && ! isInstrument (*device))
            rack->addPlugin (device, { 0.5f, 0.5f }, false);

    wireContainer (*rack);
    addContainerMacros (*rack);
    return edit.getPluginCache().createNewPlugin (te::RackInstance::create (*rack));
}

void deleteContainer (te::RackInstance& container)
{
    // The windows of the devices it holds close with it; its rack type stays for the session.
    if (container.type != nullptr)
        for (auto* device : getRackDevices (*container.type))
            device->hideWindowForShutdown();

    container.deleteFromParent();
}

te::RackType::Ptr addContainerRack (te::Edit& edit)
{
    juce::ValueTree rack (te::IDs::RACK);
    edit.createNewItemID().writeID (rack, nullptr);
    return addContainerRack (edit, rack);
}

te::RackType::Ptr addContainerRack (te::Edit& edit, const juce::ValueTree& rackState)
{
    // As Tracktion's rack list adds one, but without an undo step.
    edit.state.getOrCreateChildWithName (te::IDs::RACKS, nullptr).appendChild (rackState.createCopy(), nullptr);
    auto rack = edit.getRackList().getRackTypeForID (te::EditItemID::fromID (rackState));

    if (rack != nullptr && edit.engine.getEngineBehaviour().arePluginsRemappedWhenTempoChanges())
        rack->getMacroParameterListForWriting().remapOnTempoChange = true;

    return rack;
}

namespace
{
    struct Wire
    {
        te::EditItemID source;
        int sourcePin;
        te::EditItemID destination;
        int destinationPin;

        bool operator== (const Wire&) const = default;
    };

    std::vector<Wire> getWires (const te::RackType& rack)
    {
        std::vector<Wire> wires;

        for (const auto* connection : rack.getConnections())
            wires.push_back (
                { connection->sourceID.get(), connection->sourcePin.get(), connection->destID.get(), connection->destPin.get() });

        return wires;
    }

    // Each device takes the one before it (or the container's input), MIDI and its own two channels (not a sidechain's),
    // and the last one goes to the output.
    std::vector<Wire> getWiresInOrder (const te::RackType& rack)
    {
        std::vector<Wire> wires;
        te::EditItemID previous;                                 // the container's input,
        auto previousChannels = rack.getInputNames().size() - 1; // past its MIDI pin

        for (auto* device : getRackDevices (rack))
        {
            juce::StringArray ins, outs;
            device->getChannelNames (&ins, &outs);
            wires.push_back ({ previous, 0, device->itemID, 0 });

            for (int pin = 1; pin <= std::min ({ previousChannels, (int) ins.size(), 2 }); ++pin)
                wires.push_back ({ previous, pin, device->itemID, pin });

            previous = device->itemID;
            previousChannels = outs.size();
        }

        wires.push_back ({ previous, 0, {}, 0 });

        for (int pin = 1; pin <= std::min (previousChannels, rack.getOutputNames().size() - 1); ++pin)
            wires.push_back ({ previous, pin, {}, pin });

        return wires;
    }

    // The MIDI wires, which go through every device in its order.
    std::vector<Wire> getMidiWires (std::vector<Wire> wires)
    {
        wires.erase (std::remove_if (wires.begin(), wires.end(), [] (const Wire& wire) { return wire.sourcePin != 0; }), wires.end());
        return wires;
    }
} // namespace

void wireContainer (te::RackType& rack)
{
    // Not again while its devices are wired in their order, by the MIDI wires: how many channels a plug-in passes on is
    // unknown while it isn't loaded (a project opened with its plug-ins off), and wiring it again then would lose its audio.
    const auto wired = getMidiWires (getWires (rack)), wanted = getMidiWires (getWiresInOrder (rack));

    if (wired.size() == wanted.size()
        && std::all_of (wanted.begin(), wanted.end(),
                        [&wired] (const Wire& wire) { return std::find (wired.begin(), wired.end(), wire) != wired.end(); }))
        return;

    auto* undoManager = &rack.edit.getUndoManager();

    for (const auto& wire : getWires (rack))
        rack.removeConnection (wire.source, wire.sourcePin, wire.destination, wire.destinationPin);

    for (const auto& wire : getWiresInOrder (rack))
        rack.addConnection (wire.source, wire.sourcePin, wire.destination, wire.destinationPin);

    // Left to right in their order, where Tracktion's own view of a rack would show them.
    const auto devices = getRackDevices (rack);

    for (int i = 0; i < devices.size(); ++i)
        devices[i]->state.getParent().setProperty (te::IDs::x, (float) (i + 1) / (float) (devices.size() + 1), undoManager);
}
} // namespace phazite
