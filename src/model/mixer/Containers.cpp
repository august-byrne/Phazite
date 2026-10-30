// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Containers.h"
#include "internal/Mixer.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/DeviceStates.h"
#include "model/devices/internal/Devices.h"
#include "model/mirror/internal/Mirror.h"
#include "model/modulation/internal/Modulation.h"

#include <algorithm>

namespace phazite
{
namespace
{
    // A channel's devices: a track's, or with none the master's.
    juce::Array<te::Plugin*> getChannelDevices (te::Edit& edit, te::Track* track)
    {
        return track != nullptr ? getDevices (*track) : getMasterDevices (edit);
    }

    // Points what a state's devices are modulated by at other modulators.
    void remapModulationSources (juce::ValueTree state, const te::EditItemID::IDMap& modulators)
    {
        if (state.hasType (te::IDs::MODIFIERASSIGNMENTS))
        {
            for (auto assignment : state)
                if (const auto found = modulators.find (te::EditItemID::fromProperty (assignment, te::IDs::source));
                    found != modulators.end())
                    found->second.setProperty (assignment, te::IDs::source, nullptr);

            return;
        }

        for (auto child : state)
            remapModulationSources (child, modulators);
    }

    // Gives a track's containers that play the same rack type as another's (a copy's) rack types of their own, and those of
    // the tracks in it, noting what each device in them is in its copy.
    void copyContainersOf (te::Track& track, const te::EditItemID::IDMap& modulators, te::EditItemID::IDMap& devices)
    {
        auto& edit = track.edit;

        for (auto* plugin : track.pluginList.getPlugins())
        {
            auto* container = dynamic_cast<te::RackInstance*> (plugin);
            auto* list = container != nullptr ? container->getOwnerList() : nullptr;

            if (list == nullptr || container->type == nullptr || te::getRackInstancesInEditForType (*container->type).size() < 2)
                continue;

            // A copy of its rack type with new IDs, whose devices the copy's modulators modulate.
            storeDeviceStates (*container->type);
            auto rackState = container->type->state.createCopy();
            te::EditItemID::IDMap ids;
            te::EditItemID::remapIDs (rackState, nullptr, edit, &ids);
            remapModulationSources (rackState, modulators);
            const auto rack = addContainerRack (edit, rackState);

            if (rack == nullptr)
                continue;

            for (const auto& [from, to] : ids)
                devices[from] = to;

            // The container plays it from the same place, with its ID and settings: Tracktion makes a container's rack
            // type once, so a new container takes the old one's place.
            auto state = container->state.createCopy();
            rack->itemID.setProperty (state, te::IDs::rackType, nullptr);
            const auto index = list->indexOf (container);
            container->removeFromParent();
            list->insertPlugin (state, index);
        }

        for (auto* inside : track.getAllSubTracks (false))
            copyContainersOf (*inside, modulators, devices);
    }

    // The IDs of the rack types that containers in a saved state play.
    void findPlayedRacks (const juce::ValueTree& state, juce::Array<te::EditItemID>& played)
    {
        if (state.hasType (te::IDs::PLUGIN) && state[te::IDs::type].toString() == te::RackInstance::xmlTypeName)
            played.addIfNotAlreadyThere (te::EditItemID::fromProperty (state, te::IDs::rackType));

        for (const auto& child : state)
            findPlayedRacks (child, played);
    }
} // namespace

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

te::RackInstance* findContainerOf (const te::Plugin& device)
{
    if (! device.isInRack())
        return nullptr;

    const auto rack = device.getOwnerRackType();
    return rack != nullptr ? findContainerOf (*rack) : nullptr;
}

te::RackInstance* findContainerOf (const te::RackType& rack)
{
    return te::getRackInstancesInEditForType (rack).getFirst(); // each container plays a rack type of its own
}

juce::Array<te::Plugin*> getContainerDevices (const te::Plugin& container)
{
    auto* rack = getContainerRack (container);

    if (rack == nullptr)
        return {};

    const auto inRack = getRackDevices (*rack);
    const auto state = findDeviceState (container.edit, getDeviceID (container));

    if (! state.isValid())
        return inRack;

    juce::Array<te::Plugin*> devices;

    for (const auto& inside : getDeviceStates (state))
        for (auto* device : inRack)
            if (getDeviceID (*device) == getDeviceID (inside))
                devices.add (device);

    return devices;
}

juce::ReferenceCountedArray<te::MacroParameter> getContainerMacros (const te::Plugin& container)
{
    auto* rack = getContainerRack (container);
    return rack != nullptr ? rack->getMacroParameters() : juce::ReferenceCountedArray<te::MacroParameter>();
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

te::Track* getTrackOfContainer (const te::RackInstance& container)
{
    if (auto* track = container.getOwnerTrack())
        return track;

    auto& edit = container.edit;
    return edit.getMasterPluginList().contains (&container) ? edit.getMasterTrack() : nullptr;
}

te::Track* getTrackOfContainerDevice (const te::Plugin& device)
{
    auto* container = findContainerOf (device);
    return container != nullptr ? getTrackOfContainer (*container) : nullptr;
}

//==============================================================================
bool canGroupIntoContainer (const juce::Array<te::Plugin*>& effects)
{
    auto* channel = ! effects.isEmpty() && effects.getFirst() != nullptr ? effects.getFirst()->getOwnerList() : nullptr;

    if (channel == nullptr)
        return false;

    return std::all_of (effects.begin(), effects.end(),
                        [channel] (te::Plugin* effect)
                        {
                            return effect != nullptr && effect->getOwnerList() == channel && isDevice (*effect) && ! isContainer (*effect)
                                   && ! isInstrument (*effect) && te::RackType::isPluginAllowed (effect);
                        });
}

te::RackInstance* groupIntoContainer (const juce::Array<te::Plugin*>& effects)
{
    if (! canGroupIntoContainer (effects))
        return nullptr;

    auto& edit = effects.getFirst()->edit;
    auto* track = effects.getFirst()->getOwnerTrack(); // nullptr for the master's

    // In their order on the channel, in the first one's place.
    const auto channel = getChannelDevices (edit, track);
    te::Plugin::Array grouped;

    for (auto* device : channel)
        if (effects.contains (device))
            grouped.add (device);

    auto chain = findChainState (edit, track);
    auto first = findDeviceState (edit, getDeviceID (*grouped.getFirst()));

    if (! chain.isValid() || ! first.isValid())
        return nullptr;

    // The song's container, in the first one's place, with a new ID and name; the devices' elements go into it, with what
    // they hold of their states (which an undo brings back), and the mirror puts Tracktion's devices in its rack type.
    auto* undo = &edit.getUndoManager();
    const auto id = DeviceID::fromRaw (edit.createNewItemID().getRawID());
    juce::ValueTree container (schema::CONTAINER);
    container.setProperty (schema::deviceID, (juce::int64) id.getRaw(), nullptr);
    container.setProperty (schema::containerName, makeContainerName (edit), nullptr);

    // Its eight macros, as addContainerMacros() gives a container: "Macro 1" to "Macro 8", each at 0.
    for (int number = 1; number <= macrosPerContainer; ++number)
    {
        juce::ValueTree macro (schema::MACRO);
        macro.setProperty (schema::modulatorID, (juce::int64) edit.createNewItemID().getRawID(), nullptr);
        macro.setProperty (schema::macroName, "Macro " + juce::String (number), nullptr);
        container.appendChild (macro, nullptr);
    }

    chain.addChild (container, chain.indexOf (first), undo);

    for (auto* device : grouped)
    {
        if (device->getSidechainSourceID().isValid())
            device->setSidechainSourceID ({});

        storeDeviceState (*device);
        auto state = findDeviceState (edit, getDeviceID (*device));
        chain.removeChild (state, undo);
        container.appendChild (state, undo);
    }

    mirror::reconcileDevices (edit);

    for (auto* device : getChannelDevices (edit, track))
        if (getDeviceID (*device) == id)
            return dynamic_cast<te::RackInstance*> (device);

    return nullptr;
}

te::Plugin::Ptr createContainer (te::Edit& edit, const te::Plugin::Array& devices, const juce::String& name)
{
    const auto rack = addContainerRack (edit);

    if (rack == nullptr)
        return {};

    rack->rackName = name.isNotEmpty() ? name : makeContainerName (edit);

    for (const auto& device : devices)
        if (device != nullptr && ! isInstrument (*device))
            rack->addPlugin (device, { 0.5f, 0.5f }, false);

    wireContainer (*rack);
    addContainerMacros (*rack);
    return edit.getPluginCache().createNewPlugin (te::RackInstance::create (*rack));
}

void ungroupContainer (te::RackInstance& container)
{
    auto& edit = container.edit;
    auto state = findDeviceState (edit, getDeviceID (container));

    if (container.type == nullptr || ! state.isValid())
        return;

    // Its devices' elements go back in its place, in their order, and it goes with its macros and what they modulated; the
    // mirror moves Tracktion's devices out of its rack type, and takes away what its macros modulated.
    auto* undo = &edit.getUndoManager();
    auto chain = state.getParent();
    auto index = chain.indexOf (state);

    for (auto inside : getDeviceStates (state))
    {
        state.removeChild (inside, undo);
        chain.addChild (inside, index++, undo);
    }

    chain.removeChild (state, undo);
    mirror::reconcileDevices (edit);
}

void setContainerName (te::Plugin& container, const juce::String& name)
{
    if (auto state = findDeviceState (container.edit, getDeviceID (container)); state.hasType (schema::CONTAINER))
    {
        state.setProperty (schema::containerName, name, &container.edit.getUndoManager());
        mirror::reconcileDevices (container.edit);
    }
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

void dropUnusedContainerRacks (juce::ValueTree& editState)
{
    auto racks = editState.getChildWithName (te::IDs::RACKS);
    juce::Array<te::EditItemID> played;
    findPlayedRacks (editState, played);

    for (int i = racks.getNumChildren(); --i >= 0;)
        if (const auto rack = racks.getChild (i); rack.hasType (te::IDs::RACK) && ! played.contains (te::EditItemID::fromID (rack)))
            racks.removeChild (i, nullptr);
}

//==============================================================================
te::Plugin* insertIntoContainer (te::RackInstance& container, const te::Plugin::Ptr& device, int index)
{
    auto* rack = container.type.get();
    auto state = findDeviceState (container.edit, getDeviceID (container));

    // The model makes the device in Tracktion as it adds it to the song (T1, step 5), at the rack type's end; the mirror
    // puts it in its place and wires the row again.
    if (rack == nullptr || ! state.hasType (schema::CONTAINER) || device == nullptr || isInstrument (*device) || isContainer (*device)
        || ! rack->addPlugin (device, { 0.5f, 0.5f }, false))
        return nullptr;

    const auto devices = getDeviceStates (state);
    const auto at = juce::isPositiveAndBelow (index, (int) devices.size()) ? state.indexOf (devices[(size_t) index]) : -1;
    state.addChild (makeDeviceState (*device), at, &container.edit.getUndoManager());
    mirror::reconcileDevices (container.edit);
    return device.get();
}

void moveInContainer (te::Plugin& device, int newIndex)
{
    auto state = findDeviceState (device.edit, getDeviceID (device));
    auto container = state.getParent();

    if (! container.hasType (schema::CONTAINER))
        return;

    // To the place of the device that is there now: after it when moving later, before it when moving earlier.
    const auto devices = getDeviceStates (container);
    const auto& target = devices[(size_t) juce::jlimit (0, (int) devices.size() - 1, newIndex)];
    container.moveChild (container.indexOf (state), container.indexOf (target), &device.edit.getUndoManager());
    mirror::reconcileDevices (device.edit);
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

juce::String makeContainerName (const te::Edit& edit)
{
    juce::StringArray taken;
    auto chains = getTrackStates (edit);
    chains.push_back (findChainState (edit, nullptr));

    for (const auto& chain : chains)
        for (const auto& state : getDeviceStates (chain))
            if (state.hasType (schema::CONTAINER))
                taken.add (state[schema::containerName].toString());

    juce::String name ("Container");

    for (int number = 2; taken.contains (name); ++number)
        name = "Container " + juce::String (number);

    return name;
}

void addContainerMacros (te::RackType& rack)
{
    // Made with their values in their states, so that redoing their container's step puts them back at 0 too.
    auto& macros = rack.getMacroParameterListForWriting();
    auto& edit = rack.edit;

    for (auto number = macros.getMacroParameters().size() + 1; number <= macrosPerContainer; ++number)
    {
        juce::ValueTree macro (te::IDs::MACROPARAMETER);
        edit.createNewItemID().writeID (macro, nullptr);
        macro.setProperty (te::IDs::name, "Macro " + juce::String (number), nullptr);
        macro.setProperty (te::IDs::value, 0.0f, nullptr);
        macros.state.addChild (macro, -1, &edit.getUndoManager());
    }
}

//==============================================================================
te::EditItemID::IDMap copyContainers (const te::Track& original, te::Track& copy)
{
    te::EditItemID::IDMap modulators, devices;
    mapCopiedModulators (original, copy, modulators);
    copyContainersOf (copy, modulators, devices);
    return devices;
}
} // namespace phazite
