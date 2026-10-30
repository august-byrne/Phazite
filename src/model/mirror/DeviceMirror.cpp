// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Mirror.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/DeviceFiles.h"
#include "model/devices/internal/DevicePlugin.h"
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
    // state keeps them, and whether it is on. Nothing for another device, which the model makes while step 5 lasts.
    te::Plugin::Ptr makeOwnDevice (te::Edit& edit, const juce::ValueTree& state)
    {
        const auto type = state[schema::deviceType].toString();

        if (! state.hasType (schema::DEVICE) || ! isOwnType (type))
            return {};

        juce::ValueTree plugin (te::IDs::PLUGIN);
        plugin.setProperty (te::IDs::type, type, nullptr);
        toTracktion (getDeviceID (state)).writeID (plugin, nullptr);

        for (const auto& part : { state.getChildWithName (schema::VALUES), state.getChildWithName (schema::SOUNDS) })
            for (int i = 0; i < part.getNumProperties(); ++i)
                plugin.setProperty (part.getPropertyName (i), part[part.getPropertyName (i)], nullptr);

        if ((bool) state[schema::deviceOff])
            plugin.setProperty (te::IDs::enabled, false, nullptr);

        return edit.getPluginCache().createNewPlugin (plugin);
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
    // type kept: Containers.h), those it has go in its order (made, for one of Phazite's own), switched on or off, and
    // Phazite's own with their values and files.
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
                // A plug-in or a container is made by the model as it adds it, while step 5 lasts.
                if (const auto made = makeOwnDevice (edit, states[i]))
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
