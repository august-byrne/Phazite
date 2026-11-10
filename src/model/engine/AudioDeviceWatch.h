// SPDX-License-Identifier: AGPL-3.0-or-later
// The audio devices plugged in while Phazite runs, which it offers to switch to (the owner, 2026-11-09; the notices in the
// window's corner, ui/shell/DeviceNotices.h): what the device manager's driver lists, watched as it changes; and the switch,
// as the audio settings would make it, which Phazite's settings then keep (internal/AudioIO.h). Message thread only.
#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <functional>
#include <vector>

namespace phazite
{
// An output or an input of the driver that lists it, by its name there. A driver without inputs and outputs of their own
// (ASIO) lists each device once, as an output, which records too.
struct AudioDeviceName
{
    juce::String type, name;
    bool isInput = false;

    friend bool operator== (const AudioDeviceName&, const AudioDeviceName&) = default;
};

// What the device manager's driver lists, watched: each output and input plugged in since the watch began, unless it plays
// or records already, and each gone. What a driver lists as the watch begins, or as it is chosen in another's place, is
// there already: nothing of it was plugged in.
class AudioDeviceWatch final : private juce::ChangeListener
{
public:
    explicit AudioDeviceWatch (juce::AudioDeviceManager&);
    ~AudioDeviceWatch() override;

    // After the devices changed: those plugged in, outputs first, in the order their driver lists them, and those gone (all of
    // a driver's as another is chosen).
    std::function<void (const std::vector<AudioDeviceName>& plugged, const std::vector<AudioDeviceName>& gone)> onChange;

private:
    juce::AudioDeviceManager& devices;
    juce::String driver;                // the driver whose devices it knows
    std::vector<AudioDeviceName> known; // what it lists

    std::vector<AudioDeviceName> list() const; // what the driver chosen lists now
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    JUCE_DECLARE_NON_COPYABLE (AudioDeviceWatch)
};

// Whether the device manager plays through it (an output, or a device of a driver without inputs of their own) or records
// from it (an input) now.
bool isAudioDeviceInUse (const juce::AudioDeviceManager&, const AudioDeviceName&);

// Whether it is an output that plays through Bluetooth, which adds its own delay (AudioDevices.h: isBluetoothOutput()).
bool isBluetoothDevice (const AudioDeviceName&);

// Plays through it, or records from it, the other side as it was, as the audio settings would switch: the device chosen in
// its driver with its default channels. An error, or nothing; an error for a driver that isn't the one chosen now.
juce::String switchToAudioDevice (juce::AudioDeviceManager&, const AudioDeviceName&);
} // namespace phazite
