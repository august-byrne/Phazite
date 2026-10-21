// SPDX-License-Identifier: AGPL-3.0-or-later
// The audio devices as Phazite chooses and describes them (docs/design.md, section 20): whether an output is Bluetooth,
// and the low-latency setup a first run picks. Message thread only.
#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <optional>

namespace phazite
{
// JUCE's name for Windows' shared mode with small buffers (IAudioClient3), which Windows 10 and later offer.
inline constexpr const char* lowLatencyDeviceType = "Windows Audio (Low Latency Mode)";

// Whether an output plays through Bluetooth, whose own delay (about 150 to 250 ms) no driver reports. On Windows, the
// audio endpoint of that name is asked whether its device hangs off a Bluetooth link; elsewhere, or for a device Windows
// doesn't list by that name, isBluetoothName() decides.
bool isBluetoothOutput (const juce::String& deviceType, const juce::String& deviceName);
bool isBluetoothName (const juce::String& deviceName);

// The buffer a low-latency setup uses, of the sizes a device offers: the largest from 128 to 256 samples, or none.
std::optional<int> chooseLowLatencyBufferSize (const juce::Array<int>& available);

// For a first run (no audio setup saved yet): Windows' Low Latency Mode with chooseLowLatencyBufferSize()'s buffer, on
// the default output, where it offers one; otherwise the setup it had. Either way the setup becomes the chosen one, which
// the Engine saves, so later starts keep it. Returns whether it switched to Low Latency Mode.
bool chooseLowLatencySetup (juce::AudioDeviceManager&);
} // namespace phazite
