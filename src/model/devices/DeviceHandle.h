// SPDX-License-Identifier: AGPL-3.0-or-later
// One of Phazite's own devices as its editor uses it: its description, a handle to each of its parameters, and its DSP
// for what an editor reads without waiting (an EQ's analyzer tap, a compressor's meter). It finds its device each
// time, so it is safe to keep after the device is deleted: it then has none.
#pragma once

#include "ParameterHandle.h"
#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"
#include "model/base/Tracktion.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace phazite
{
class DeviceHandle
{
public:
    DeviceHandle() = default;

    // Of a plug-in: empty unless it is one of Phazite's own devices.
    explicit DeviceHandle (te::Plugin&);

    bool isValid() const;
    const devices::DeviceInfo* getInfo() const; // nullptr without a device

    // The parameter with that ID; its changes are undo steps named "Change" and its name, or undoName.
    ParameterHandle getParameter (std::string_view id, const juce::String& undoName = {}) const;

    // A track a device's sidechain can listen to (model/mixer/Sidechain.h), by its ID, which stays the same while the
    // project is open; 0 for none, the sound passing through the device.
    struct SidechainChoice
    {
        std::uint64_t trackID = 0;
        juce::String name;
    };

    std::vector<SidechainChoice> getSidechainChoices() const; // in the song's order
    SidechainChoice getSidechainSource() const;
    void setSidechainSource (std::uint64_t trackID) const; // one of the choices, or 0; as one undo step

    // The drum sampler's pads (model/devices/DrumKit.h): what each plays, and putting a sound file on one, or the kit's
    // sound back, as undo steps. setPadSound() returns false for a file that isn't a sound.
    juce::String getPadName (int pad) const;
    bool hasPadSound (int pad) const;
    bool setPadSound (int pad, const juce::File&) const;
    void clearPadSound (int pad) const;

    // The device's DSP as its own type, for an editor's lock-free reads; nullptr once the device is gone, or for another
    // type. Ask each time: never keep the pointer.
    template <typename DeviceProcessor>
    DeviceProcessor* getProcessor() const
    {
        return dynamic_cast<DeviceProcessor*> (getProcessorBase());
    }

private:
    te::SafeSelectable<te::Plugin> plugin;

    devices::Processor* getProcessorBase() const;
};
} // namespace phazite
