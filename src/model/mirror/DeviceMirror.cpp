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

        if (state.hasProperty (schema::pluginMix))
        {
            const auto mix = (float) state[schema::pluginMix];
            plugin.setProperty (te::IDs::wet, mix, nullptr);
            plugin.setProperty (te::IDs::dry, 1.0f - mix, nullptr);
        }

        return edit.getPluginCache().createNewPlugin (plugin);
    }

    // A device the song has and Tracktion hasn't, made as the song has it. Nothing for a container, which the model makes as
    // it adds one while step 5 lasts, nor for one of Tracktion's own, which only tests make.
    te::Plugin::Ptr makeDevice (te::Edit& edit, const juce::ValueTree& state)
    {
        if (isPluginState (state))
            return makePlugin (edit, state);

        if (state.hasType (schema::DEVICE) && isOwnType (state[schema::deviceType].toString()))
            return makeOwnDevice (edit, state);

        return {};
    }

    // A plug-in plays the song's mix, as the dry and wet levels Tracktion gives it, set by hand (setMix() in Mixer.h).
    void reconcilePlugin (te::ExternalPlugin& plugin, const juce::ValueTree& state)
    {
        const auto mix = (float) state.getProperty (schema::pluginMix, 1.0f);
        const auto play = [] (te::AutomatableParameter* level, float wanted)
        {
            if (level != nullptr && ! juce::exactlyEqual (level->getCurrentExplicitValue(), wanted))
                level->setParameter (wanted, juce::sendNotification);
        };

        play (plugin.wetGain.get(), mix);
        play (plugin.dryGain.get(), 1.0f - mix);
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

    // A chain's devices in Tracktion as the song has them: those it hasn't go (a container with what it holds, its rack
    // type kept: Containers.h), those it has go in its order (made, but for a container), switched on or off, Phazite's
    // own with their values and files, and plug-ins with their mixes.
    void reconcileChain (te::Edit& edit, te::PluginList& list, const juce::ValueTree& chain)
    {
        auto* undo = &edit.getUndoManager();
        const auto states = getDeviceStates (chain);
        std::vector<DeviceID> wanted;

        for (const auto& state : states)
            wanted.push_back (getDeviceID (state));

        for (auto* device : getTracktionDevices (list))
        {
            if (std::find (wanted.begin(), wanted.end(), getDeviceID (*device)) != wanted.end())
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
            te::Plugin* device = nullptr;

            for (auto* each : devices)
                if (getDeviceID (*each) == wanted[i])
                    device = each;

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

            if (const auto on = ! (bool) states[i][schema::deviceOff]; device->isEnabled() != on)
                device->setEnabled (on);

            if (auto* own = dynamic_cast<DevicePlugin*> (device))
                reconcileOwnDevice (*own, states[i]);
            else if (auto* plugin = dynamic_cast<te::ExternalPlugin*> (device); plugin != nullptr && isPluginState (states[i]))
                reconcilePlugin (*plugin, states[i]);
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
