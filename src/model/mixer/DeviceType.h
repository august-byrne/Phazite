// SPDX-License-Identifier: AGPL-3.0-or-later
// A device that can be added (docs/design.md, section 9): one of Phazite's own (model/devices/internal/Devices.h) or a plug-in
// found by scanning, as the device menus, the browser and the plug-in picker list them. The model makes them
// (Mixer.h: getDeviceTypes(); MixerModel.h).
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace phazite
{
struct DeviceType
{
    juce::String name;
    juce::String category; // "Instrument" or "Effect"
    juce::PluginDescription description;
    bool isInstrument = false;
    bool isBuiltIn = false; // one of Phazite's own, rather than a plug-in
};

// A plug-in description's device: an instrument or an effect, named as the description names it.
DeviceType makeDeviceType (const juce::PluginDescription&);
} // namespace phazite
