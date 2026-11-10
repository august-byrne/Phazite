// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AudioDeviceWatch.h"
#include "AudioDevices.h"

#include <algorithm>
#include <utility>

namespace phazite
{
AudioDeviceWatch::AudioDeviceWatch (juce::AudioDeviceManager& manager)
    : devices (manager),
      driver (manager.getCurrentAudioDeviceType()),
      known (list())
{
    devices.addChangeListener (this);
}

AudioDeviceWatch::~AudioDeviceWatch()
{
    devices.removeChangeListener (this);
}

std::vector<AudioDeviceName> AudioDeviceWatch::list() const
{
    std::vector<AudioDeviceName> names;

    if (auto* type = devices.getCurrentDeviceTypeObject())
    {
        const auto separate = type->hasSeparateInputsAndOutputs();

        for (const auto isInput : { false, true })
            if (! isInput || separate)
                for (const auto& name : type->getDeviceNames (isInput))
                    names.push_back ({ type->getTypeName(), name, isInput });
    }

    return names;
}

void AudioDeviceWatch::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // The device manager tells of every change (a device opened, a setting changed, the lists): only its lists matter here.
    auto now = list();
    const auto type = devices.getCurrentAudioDeviceType();
    const auto lists = [] (const std::vector<AudioDeviceName>& names, const AudioDeviceName& name)
    { return std::find (names.begin(), names.end(), name) != names.end(); };

    std::vector<AudioDeviceName> plugged, gone;

    for (const auto& each : known)
        if (! lists (now, each))
            gone.push_back (each);

    // Another driver chosen: what it lists is there already.
    if (type == driver)
        for (const auto& each : now)
            if (! lists (known, each) && ! isAudioDeviceInUse (devices, each))
                plugged.push_back (each);

    driver = type;
    known = std::move (now);

    if ((! plugged.empty() || ! gone.empty()) && onChange != nullptr)
        onChange (plugged, gone);
}

bool isAudioDeviceInUse (const juce::AudioDeviceManager& devices, const AudioDeviceName& device)
{
    if (devices.getCurrentAudioDevice() == nullptr || devices.getCurrentAudioDeviceType() != device.type)
        return false;

    const auto setup = devices.getAudioDeviceSetup();
    return (device.isInput ? setup.inputDeviceName : setup.outputDeviceName) == device.name;
}

bool isBluetoothDevice (const AudioDeviceName& device)
{
    return ! device.isInput && isBluetoothOutput (device.type, device.name);
}

juce::String switchToAudioDevice (juce::AudioDeviceManager& devices, const AudioDeviceName& device)
{
    auto* type = devices.getCurrentDeviceTypeObject();

    if (type == nullptr || type->getTypeName() != device.type)
        return "The audio settings use another driver now than " + device.type + ".";

    // As the audio settings' lists of devices switch (JUCE's AudioDeviceSelectorComponent): the side chosen anew on its
    // default channels, a device of a driver without inputs of their own on both sides.
    auto setup = devices.getAudioDeviceSetup();

    if (device.isInput)
    {
        setup.inputDeviceName = device.name;
        setup.useDefaultInputChannels = true;
    }
    else
    {
        setup.outputDeviceName = device.name;
        setup.useDefaultOutputChannels = true;
    }

    if (! type->hasSeparateInputsAndOutputs())
        setup.inputDeviceName = setup.outputDeviceName;

    return devices.setAudioDeviceSetup (setup, true);
}
} // namespace phazite
