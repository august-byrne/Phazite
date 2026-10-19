// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Containers.h"
#include "internal/Mixer.h"
#include "model/devices/internal/DeviceStates.h"
#include "model/devices/internal/Devices.h"

#include <algorithm>

namespace phazite
{
namespace
{
    // A rack type's devices, in the order of their places in its state: the order the sound goes through them.
    juce::Array<te::Plugin*> getDevicesOf (const te::RackType& rack)
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

    // A channel's devices: a track's, or with none the master's.
    juce::Array<te::Plugin*> getChannelDevices (te::Edit& edit, te::Track* track)
    {
        return track != nullptr ? getDevices (*track) : getMasterDevices (edit);
    }

    te::Plugin* insertOnChannel (te::Edit& edit, te::Track* track, const te::Plugin::Ptr& device, int index)
    {
        return track != nullptr ? insertDevice (*track, device, index) : insertMasterDevice (edit, device, index);
    }

    // What each of a track's modulators and macros, and those of the tracks in it, is in a copy of it: the one in the
    // same place.
    void mapModulators (const te::Track& original, const te::Track& copy, te::EditItemID::IDMap& map)
    {
        auto* originals = original.getModifierList();
        auto* copies = copy.getModifierList();

        if (originals != nullptr && copies != nullptr)
        {
            const auto from = originals->getModifiers(), to = copies->getModifiers();

            for (int i = 0; i < from.size() && i < to.size(); ++i)
                map[from[i]->itemID] = to[i]->itemID;
        }

        auto* originalTrack = dynamic_cast<const te::AudioTrack*> (&original);
        auto* copiedTrack = dynamic_cast<const te::AudioTrack*> (&copy);

        if (originalTrack != nullptr && copiedTrack != nullptr)
        {
            const auto from = originalTrack->getMacroParameters(), to = copiedTrack->getMacroParameters();

            for (int i = 0; i < from.size() && i < to.size(); ++i)
                map[te::EditItemID::fromID (from[i]->state)] = te::EditItemID::fromID (to[i]->state);
        }

        const auto originalTracks = original.getAllSubTracks (false), copiedTracks = copy.getAllSubTracks (false);

        for (int i = 0; i < originalTracks.size() && i < copiedTracks.size(); ++i)
            mapModulators (*originalTracks[i], *copiedTracks[i], map);
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

    // What a container's macros modulate: its devices' parameters.
    void removeMacroModulations (te::RackType& rack)
    {
        for (auto* macro : rack.getMacroParameters())
            for (auto* parameter : te::getAllParametersBeingModifiedBy (rack.edit, *macro))
                parameter->removeModifier (*macro);
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
    return rack != nullptr ? getDevicesOf (*rack) : juce::Array<te::Plugin*>();
}

juce::ReferenceCountedArray<te::MacroParameter> getContainerMacros (const te::Plugin& container)
{
    auto* rack = getContainerRack (container);
    return rack != nullptr ? rack->getMacroParameters() : juce::ReferenceCountedArray<te::MacroParameter>();
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

    const auto index = channel.indexOf (grouped.getFirst().get());
    const auto name = makeContainerName (edit);
    const auto rack = addContainerRack (edit);

    if (rack == nullptr)
        return nullptr;

    rack->rackName = name;

    for (auto* device : grouped)
    {
        if (device->getSidechainSourceID().isValid())
            device->setSidechainSourceID ({});

        storeDeviceState (*device);
        rack->addPlugin (device, { 0.5f, 0.5f }, false);
    }

    wireContainer (*rack);
    addContainerMacros (*rack);

    const auto container = edit.getPluginCache().createNewPlugin (te::RackInstance::create (*rack));
    return dynamic_cast<te::RackInstance*> (insertOnChannel (edit, track, container, index));
}

void ungroupContainer (te::RackInstance& container)
{
    const te::RackType::Ptr rack = container.type;

    if (rack == nullptr || container.getOwnerList() == nullptr)
        return;

    auto& edit = container.edit;
    auto* track = container.getOwnerTrack();
    auto index = getChannelDevices (edit, track).indexOf (&container);

    // Its devices go back in its place, in their order, without what its macros modulated, before it goes.
    removeMacroModulations (*rack);
    te::Plugin::Array devices;

    for (auto* device : getDevicesOf (*rack))
        devices.add (device);

    for (auto* device : devices)
    {
        device->removeFromParent();
        insertOnChannel (edit, track, device, index++);
    }

    container.deleteFromParent();
}

void deleteContainer (te::RackInstance& container)
{
    // Its devices' windows close with it; its rack type stays for the session.
    if (container.type != nullptr)
        for (auto* device : getDevicesOf (*container.type))
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

    if (rack == nullptr || device == nullptr || isInstrument (*device) || isContainer (*device)
        || ! rack->addPlugin (device, { 0.5f, 0.5f }, false)) // at the end
        return nullptr;

    const auto devices = getDevicesOf (*rack);

    if (juce::isPositiveAndBelow (index, devices.size() - 1))
        moveInContainer (*device, index);
    else
        wireContainer (*rack);

    return device.get();
}

void moveInContainer (te::Plugin& device, int newIndex)
{
    const auto rack = device.getOwnerRackType();

    if (rack == nullptr)
        return;

    // To the place of the device that is there now: after it when moving later, before it when moving earlier.
    const auto devices = getDevicesOf (*rack);
    const auto from = devices.indexOf (&device);
    const auto to = juce::jlimit (0, devices.size() - 1, newIndex);

    if (from >= 0 && from != to)
    {
        auto& state = rack->state;
        state.moveChild (state.indexOf (device.state.getParent()), state.indexOf (devices[to]->state.getParent()),
                         &device.edit.getUndoManager());
    }

    wireContainer (*rack);
}

void deleteFromContainer (te::Plugin& device)
{
    const auto rack = device.getOwnerRackType();
    device.deleteFromParent();

    if (rack != nullptr)
        wireContainer (*rack);
}

void wireContainer (te::RackType& rack)
{
    auto* undoManager = &rack.edit.getUndoManager();

    // Every connection goes; then each device takes the one before it (or the container's input), MIDI and its own two
    // channels (not a sidechain's), and the last one goes to the output.
    struct Wire
    {
        te::EditItemID source;
        int sourcePin;
        te::EditItemID destination;
        int destinationPin;
    };

    std::vector<Wire> wires;

    for (const auto* connection : rack.getConnections())
        wires.push_back ({ connection->sourceID.get(), connection->sourcePin.get(), connection->destID.get(), connection->destPin.get() });

    for (const auto& wire : wires)
        rack.removeConnection (wire.source, wire.sourcePin, wire.destination, wire.destinationPin);

    const auto devices = getDevicesOf (rack);
    te::EditItemID previous;                                 // the container's input,
    auto previousChannels = rack.getInputNames().size() - 1; // past its MIDI pin

    for (int i = 0; i < devices.size(); ++i)
    {
        auto* device = devices[i];
        juce::StringArray ins, outs;
        device->getChannelNames (&ins, &outs);
        rack.addConnection (previous, 0, device->itemID, 0);

        for (int pin = 1; pin <= std::min ({ previousChannels, (int) ins.size(), 2 }); ++pin)
            rack.addConnection (previous, pin, device->itemID, pin);

        // Left to right in their order, where Tracktion's own view of a rack would show them.
        device->state.getParent().setProperty (te::IDs::x, (float) (i + 1) / (float) (devices.size() + 1), undoManager);
        previous = device->itemID;
        previousChannels = outs.size();
    }

    rack.addConnection (previous, 0, {}, 0);

    for (int pin = 1; pin <= std::min (previousChannels, rack.getOutputNames().size() - 1); ++pin)
        rack.addConnection (previous, pin, {}, pin);
}

juce::String makeContainerName (const te::Edit& edit)
{
    juce::StringArray taken;

    for (auto* rack : edit.getRackList().getTypes())
        taken.add (rack->rackName.get());

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
    mapModulators (original, copy, modulators);
    copyContainersOf (copy, modulators, devices);
    return devices;
}
} // namespace phazite
