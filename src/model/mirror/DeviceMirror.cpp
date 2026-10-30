// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Mirror.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
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

    // A chain's devices in Tracktion as the song has them: those it hasn't go (a container with what it holds, its rack
    // type kept: Containers.h), those it has go in its order, switched on or off.
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
                continue; // made by the model as it adds it, while step 5 lasts

            if (const auto place = (int) i; place < devices.size() && devices[place] != device)
            {
                auto parent = device->state.getParent();
                parent.moveChild (parent.indexOf (device->state), parent.indexOf (devices[place]->state), undo);
            }

            if (const auto on = ! (bool) states[i][schema::deviceOff]; device->isEnabled() != on)
                device->setEnabled (on);
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
