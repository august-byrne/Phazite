// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Mirror.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/DeviceFiles.h"
#include "model/devices/internal/DevicePlugin.h"
#include "model/devices/internal/DeviceStates.h"
#include "model/devices/internal/Devices.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"

#include <algorithm>

namespace phazite::mirror
{
namespace
{
    juce::Array<te::Plugin*> getTracktionDevices (const te::PluginList& list)
    {
        juce::Array<te::Plugin*> devices;

        for (auto* plugin : list)
            if (isDevice (*plugin))
                devices.add (plugin);

        return devices;
    }

    te::Plugin* findByID (const juce::Array<te::Plugin*>& devices, DeviceID id)
    {
        for (auto* device : devices)
            if (getDeviceID (*device) == id)
                return device;

        return nullptr;
    }

    bool isWanted (const std::vector<juce::ValueTree>& states, DeviceID id)
    {
        return std::any_of (states.begin(), states.end(), [id] (const juce::ValueTree& state) { return getDeviceID (state) == id; });
    }

    // The dry and wet levels of a plug-in or a container, in the state Tracktion makes it from, as the song's mix sets them
    // (setMix() in Mixer.h).
    void writeMix (juce::ValueTree& plugin, const juce::ValueTree& state)
    {
        if (state.hasProperty (schema::deviceMix))
        {
            const auto mix = (float) state[schema::deviceMix];
            plugin.setProperty (te::IDs::wet, mix, nullptr);
            plugin.setProperty (te::IDs::dry, 1.0f - mix, nullptr);
        }
    }

    bool isOwnType (const juce::String& type)
    {
        for (const auto* info : getOwnDevices())
            if (type == juce::String (info->typeID.data(), info->typeID.size()))
                return true;

        return false;
    }

    // One of Phazite's own devices made as the song has it: its type and ID, its values set by hand and its files as its
    // state keeps them, and whether it is on.
    te::Plugin::Ptr makeOwnDevice (te::Edit& edit, const juce::ValueTree& state)
    {
        juce::ValueTree plugin (te::IDs::PLUGIN);
        plugin.setProperty (te::IDs::type, state[schema::deviceType].toString(), nullptr);
        toTracktion (getDeviceID (state)).writeID (plugin, nullptr);

        for (const auto& part : { state.getChildWithName (schema::VALUES), state.getChildWithName (schema::SOUNDS) })
            for (int i = 0; i < part.getNumProperties(); ++i)
                plugin.setProperty (part.getPropertyName (i), part[part.getPropertyName (i)], nullptr);

        if ((bool) state[schema::deviceOff])
            plugin.setProperty (te::IDs::enabled, false, nullptr);

        return edit.getPluginCache().createNewPlugin (plugin);
    }

    // A plug-in made as the song has it: which plug-in it is (one Tracktion can't load if it isn't installed), its ID,
    // whether it is on, its own state, program and channel layout, its values set by hand under automation, and its mix.
    te::Plugin::Ptr makePlugin (te::Edit& edit, const juce::ValueTree& state)
    {
        auto plugin = te::ExternalPlugin::create (edit.engine, getPluginDescription (edit.engine, state));
        toTracktion (getDeviceID (state)).writeID (plugin, nullptr);

        if ((bool) state[schema::deviceOff])
            plugin.setProperty (te::IDs::enabled, false, nullptr);

        if (const auto settings = state[schema::pluginState].toString(); settings.isNotEmpty())
            plugin.setProperty (te::IDs::state, settings, nullptr);

        if (state.hasProperty (schema::pluginProgram))
            plugin.setProperty (te::IDs::programNum, (int) state[schema::pluginProgram], nullptr);

        if (const auto layout = makeTracktionLayout (state); layout.getSize() > 0)
            plugin.setProperty (te::IDs::layout, layout, nullptr);

        if (const auto values = makeTracktionValues (state); values.getSize() > 0)
            plugin.setProperty (te::IDs::parameters, values, nullptr);

        writeMix (plugin, state);
        return edit.getPluginCache().createNewPlugin (plugin);
    }

    // A device the song has and Tracktion hasn't, made as the song has it. Nothing for one of Tracktion's own, which only
    // tests make (a container is made by makeContainer()).
    te::Plugin::Ptr makeDevice (te::Edit& edit, const juce::ValueTree& state)
    {
        if (isPluginState (state))
            return makePlugin (edit, state);

        if (state.hasType (schema::DEVICE) && isOwnType (state[schema::deviceType].toString()))
            return makeOwnDevice (edit, state);

        return {};
    }

    // A plug-in or a container plays the song's mix, as the dry and wet levels Tracktion gives it, set by hand.
    void reconcileMix (te::AutomatableParameter* wet, te::AutomatableParameter* dry, const juce::ValueTree& state)
    {
        const auto mix = (float) state.getProperty (schema::deviceMix, 1.0f);
        const auto play = [] (te::AutomatableParameter* level, float wanted)
        {
            if (level != nullptr && ! juce::exactlyEqual (level->getCurrentExplicitValue(), wanted))
                level->setParameter (wanted, juce::sendNotification);
        };

        play (wet, mix);
        play (dry, 1.0f - mix);
    }

    // One of Phazite's own devices plays the song's values set by hand (a value the song hasn't is the parameter's
    // default) and its files.
    void reconcileOwnDevice (DevicePlugin& device, const juce::ValueTree& state)
    {
        const auto& info = device.getDeviceInfo();
        const auto values = state.getChildWithName (schema::VALUES);

        for (const auto& description : info.parameters)
        {
            const auto wanted = (float) values.getProperty (juce::Identifier (juce::String (description.id.data(), description.id.size())),
                                                            description.defaultValue);

            if (auto* parameter = device.findParameter (description.id);
                parameter != nullptr && ! juce::exactlyEqual (parameter->getCurrentExplicitValue(), wanted))
                parameter->setParameter (wanted, juce::sendNotification);
        }

        auto* undo = &device.edit.getUndoManager();
        const auto sounds = state.getChildWithName (schema::SOUNDS);

        for (const auto& property : getSoundProperties (info))
        {
            if (const auto wanted = sounds[property].toString(); device.state[property].toString() != wanted)
            {
                if (wanted.isEmpty())
                    device.state.removeProperty (property, undo);
                else
                    device.state.setProperty (property, wanted, undo);
            }
        }
    }

    // What a device is set to, as the song has it: on or off, Phazite's own device's values and files, a plug-in's or a
    // container's mix.
    void reconcileSettings (te::Plugin& device, const juce::ValueTree& state)
    {
        if (const auto on = ! (bool) state[schema::deviceOff]; device.isEnabled() != on)
            device.setEnabled (on);

        if (auto* own = dynamic_cast<DevicePlugin*> (&device))
            reconcileOwnDevice (*own, state);
        else if (auto* plugin = dynamic_cast<te::ExternalPlugin*> (&device); plugin != nullptr && isPluginState (state))
            reconcileMix (plugin->wetGain.get(), plugin->dryGain.get(), state);
        else if (auto* container = dynamic_cast<te::RackInstance*> (&device); container != nullptr && state.hasType (schema::CONTAINER))
            reconcileMix (container->wetGain.get(), container->dryGain.get(), state);
    }

    // A container's devices in Tracktion as the song has them (Schema.h: CONTAINER), in its rack type: each found there, or
    // on the channel it is grouped from (and moved in), or made; those the song hasn't go; in the song's order, wired so;
    // each with its settings; and the container's name.
    void reconcileInside (te::Edit& edit, te::PluginList& list, te::RackType& rack, const juce::ValueTree& container)
    {
        auto* undo = &edit.getUndoManager();
        const auto states = getDeviceStates (container);

        for (const auto& state : states)
        {
            if (findByID (getRackDevices (rack), getDeviceID (state)) != nullptr)
                continue;

            te::Plugin::Ptr device = findByID (getTracktionDevices (list), getDeviceID (state));

            if (device == nullptr)
                device = makeDevice (edit, state);

            if (device != nullptr)
                rack.addPlugin (device, { 0.5f, 0.5f }, false); // out of the channel's list, for one grouped
        }

        for (auto* device : getRackDevices (rack))
            if (! isWanted (states, getDeviceID (*device)))
                device->deleteFromParent();

        // Each in its place: to the place of the device that is there now, among the rack type's other elements.
        for (size_t i = 0; i < states.size(); ++i)
        {
            const auto devices = getRackDevices (rack);
            auto* device = findByID (devices, getDeviceID (states[i]));

            if (device == nullptr)
                continue;

            if (const auto place = (int) i; place < devices.size() && devices[place] != device)
                rack.state.moveChild (rack.state.indexOf (device->state.getParent()),
                                      rack.state.indexOf (devices[place]->state.getParent()), undo);

            reconcileSettings (*device, states[i]);
        }

        wireContainer (rack);

        if (const auto name = container[schema::containerName].toString(); rack.rackName.get() != name)
            rack.rackName = name;
    }

    // A container made as the song has it: a rack type of its own for the session (Containers.h), holding the song's
    // devices (each found on the channel it is grouped from, or made), wired, with its name and eight macros (Tracktion's
    // while T1 step 6 lasts); and Tracktion's container that plays it, with the song's ID, switch and mix.
    te::Plugin::Ptr makeContainer (te::Edit& edit, te::PluginList& list, const juce::ValueTree& state)
    {
        const auto rack = addContainerRack (edit);

        if (rack == nullptr)
            return {};

        reconcileInside (edit, list, *rack, state);
        addContainerMacros (*rack);

        auto container = te::RackInstance::create (*rack);
        toTracktion (getDeviceID (state)).writeID (container, nullptr);

        if ((bool) state[schema::deviceOff])
            container.setProperty (te::IDs::enabled, false, nullptr);

        writeMix (container, state);
        return edit.getPluginCache().createNewPlugin (container);
    }

    // A chain's devices in Tracktion as the song has them. First, what the song has on the channel comes out of Tracktion's
    // containers (one ungrouped), and its containers are made or follow it, taking in what the song has them hold from the
    // channel (devices grouped); then what it hasn't on the channel goes (a container with what it still holds, its rack
    // type kept: Containers.h); then each goes in its place (made, for a device), with its settings.
    void reconcileChain (te::Edit& edit, te::PluginList& list, const juce::ValueTree& chain)
    {
        auto* undo = &edit.getUndoManager();
        const auto states = getDeviceStates (chain);

        for (auto* device : getTracktionDevices (list))
        {
            if (auto* rack = getContainerRack (*device))
            {
                for (auto* inside : getRackDevices (*rack))
                {
                    if (isWanted (states, getDeviceID (*inside)))
                    {
                        const te::Plugin::Ptr moved (inside);
                        moved->removeFromParent();
                        list.insertPlugin (moved, getDevicePlace (list, -1), nullptr);
                    }
                }
            }
        }

        for (size_t i = 0; i < states.size(); ++i)
        {
            if (! states[i].hasType (schema::CONTAINER))
                continue;

            if (auto* container = dynamic_cast<te::RackInstance*> (findByID (getTracktionDevices (list), getDeviceID (states[i]))))
            {
                if (container->type != nullptr)
                    reconcileInside (edit, list, *container->type, states[i]);
            }
            else if (const auto made = makeContainer (edit, list, states[i]))
            {
                list.insertPlugin (made, getDevicePlace (list, (int) i), nullptr);
            }
        }

        for (auto* device : getTracktionDevices (list))
        {
            if (isWanted (states, getDeviceID (*device)))
                continue;

            if (auto* container = dynamic_cast<te::RackInstance*> (device))
                deleteContainer (*container);
            else
                device->deleteFromParent();
        }

        // Each in its place: to where the device that is there now is (others of Tracktion's, such as sends and clips, can
        // sit between them in the track's state).
        for (size_t i = 0; i < states.size(); ++i)
        {
            const auto devices = getTracktionDevices (list);
            auto* device = findByID (devices, getDeviceID (states[i]));

            if (device == nullptr)
            {
                if (const auto made = makeDevice (edit, states[i]))
                    list.insertPlugin (made, getDevicePlace (list, (int) i), nullptr);

                continue;
            }

            if (const auto place = (int) i; place < devices.size() && devices[place] != device)
            {
                auto parent = device->state.getParent();
                parent.moveChild (parent.indexOf (device->state), parent.indexOf (devices[place]->state), undo);
            }

            reconcileSettings (*device, states[i]);
        }
    }
} // namespace

void reconcileDevices (te::Edit& edit)
{
    for (const auto& state : getTrackStates (edit))
        if (auto* track = te::findTrackForID (edit, toTracktion (getTrackID (state))))
            reconcileChain (edit, track->pluginList, state);

    if (const auto master = findChainState (edit, nullptr); master.isValid())
        reconcileChain (edit, edit.getMasterPluginList(), master);
}
} // namespace phazite::mirror
