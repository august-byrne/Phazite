// SPDX-License-Identifier: AGPL-3.0-or-later
// One of Phazite's own devices as its editor uses it: its description, a handle to each of its parameters, and its DSP
// for what an editor reads without waiting (an EQ's analyzer tap, a compressor's meter). It finds its device each
// time, so it is safe to keep after the device is deleted: it then has none.
#pragma once

#include "ParameterHandle.h"
#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"
#include "model/base/Tracktion.h"

#include <string_view>

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
