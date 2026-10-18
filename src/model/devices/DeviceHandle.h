// SPDX-License-Identifier: AGPL-3.0-or-later
// One of Phazite's own devices as its editor uses it: its description, a handle to each of its parameters, and its DSP
// for what an editor reads without waiting (an EQ's analyzer tap, a compressor's meter). It finds its device each
// time, so it is safe to keep, and to copy, after the device is deleted: it then has none. The model hands handles out
// (internal/DeviceHandle.h, for the app's device windows).
#pragma once

#include "ParameterHandle.h"
#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"
#include "devices/wavetable/Wavetable.h"
#include "model/base/IDs.h"

#include <memory>
#include <string_view>
#include <vector>

namespace phazite
{
class DeviceHandle
{
public:
    DeviceHandle() = default; // of nothing

    bool isValid() const;
    const devices::DeviceInfo* getInfo() const; // nullptr without a device

    // The parameter with that ID; its changes are undo steps named "Change" and its name, or undoName.
    ParameterHandle getParameter (std::string_view id, const juce::String& undoName = {}) const;

    // A track a device's sidechain can listen to (model/mixer/internal/Sidechain.h), or none: the sound passing through the
    // device.
    struct SidechainChoice
    {
        TrackID track;
        juce::String name;
    };

    std::vector<SidechainChoice> getSidechainChoices() const; // in the song's order
    SidechainChoice getSidechainSource() const;
    void setSidechainSource (TrackID) const; // one of the choices, or none; as one undo step

    // The drum sampler's pads (model/devices/internal/DrumKit.h): what each plays, and putting a sound file on one, or the kit's
    // sound back, as undo steps. setPadSound() returns false for a file that isn't a sound.
    juce::String getPadName (int pad) const;
    bool hasPadSound (int pad) const;
    bool setPadSound (int pad, const juce::File&) const;
    void clearPadSound (int pad) const;

    // The wavetable synth's oscillators (model/devices/internal/Wavetables.h): what each plays, its table for drawing, and
    // putting one of the factory tables (devices/wavetable/FactoryWavetables.h) or a sound file on one, as undo steps.
    // setWavetableFile() returns false for a file that isn't a sound.
    juce::String getWavetableName (int osc) const;
    bool hasWavetableFile (int osc) const;
    std::shared_ptr<const devices::Wavetable> getWavetable (int osc) const;
    void setFactoryWavetable (int osc, int index) const;
    bool setWavetableFile (int osc, const juce::File&) const;

    // The device's DSP as its own type, for an editor's lock-free reads; nullptr once the device is gone, or for another
    // type. Ask each time: never keep the pointer.
    template <typename DeviceProcessor>
    DeviceProcessor* getProcessor() const
    {
        return dynamic_cast<DeviceProcessor*> (getProcessorBase());
    }

    struct Impl; // the device
    explicit DeviceHandle (std::shared_ptr<const Impl>);

private:
    std::shared_ptr<const Impl> impl;

    devices::Processor* getProcessorBase() const;
};
} // namespace phazite
