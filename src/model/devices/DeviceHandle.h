// SPDX-License-Identifier: AGPL-3.0-or-later
// One of Phazite's own devices as its editor uses it: its description, a handle to each of its parameters, and its DSP
// for what an editor reads without waiting (an EQ's analyzer tap, a compressor's meter). It finds its device each
// time, so it is safe to keep, and to copy, after the device is deleted: it then has none. The model hands handles out
// (internal/DeviceHandle.h, for the app's device windows).
#pragma once

#include "ParameterHandle.h"
#include "devices/core/DeviceInfo.h"
#include "devices/core/DeviceScope.h"
#include "devices/core/Processor.h"
#include "devices/sampler/Sampler.h"
#include "devices/wavetable/Wavetable.h"
#include "model/base/IDs.h"
#include "model/mixer/SidechainChoice.h"

#include <memory>
#include <optional>
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

    // The track its sidechain listens to, or none.
    SidechainChoice getSidechainSource() const;

    // The drum sampler's pads by their notes, and their layers (model/devices/internal/DrumKit.h): what each plays, whether it
    // plays a file or the pad nothing, and putting a sound file on one, or taking it off, as undo steps. setPadSound() returns
    // false for a file that isn't a sound.
    juce::String getPadName (int pad, int layer = 0) const;
    bool hasPadSound (int pad, int layer = 0) const;
    bool isPadEmpty (int pad) const;
    bool setPadSound (int pad, const juce::File&, int layer = 0) const;
    void clearPadSound (int pad, int layer = 0) const;

    // The track that plays a drum sampler's pad on a mixer channel of its own, or none; and giving the pad one, or bringing it
    // back to the sampler's track, deleting its track, each as an undo step.
    TrackID getPadTrack (int pad) const;
    void setPadOnOwnTrack (int pad, bool) const;

    // The swap open on the device (model/devices/SwapModel.h): the pad it tries sounds on, or -1 while it tries the device's
    // presets; none while none is.
    std::optional<int> getSwap() const;

    // The wavetable synth's oscillators (model/devices/internal/Wavetables.h): what each plays, its table for drawing, and
    // putting one of the factory tables (devices/wavetable/FactoryWavetables.h) or a sound file on one, as undo steps.
    // setWavetableFile() returns false for a file that isn't a sound.
    juce::String getWavetableName (int osc) const;
    bool hasWavetableFile (int osc) const;
    std::shared_ptr<const devices::Wavetable> getWavetable (int osc) const;
    void setFactoryWavetable (int osc, int index) const;
    bool setWavetableFile (int osc, const juce::File&) const;

    // The sampler's sound (model/devices/internal/SamplerSound.h): what it plays, which factory sound (-1 while a file), and
    // putting a sound file or one of its factory sounds on it, and finding its root key again, as undo steps; where it is
    // sliced, as parts of the sound's length, and its slices moved, added and taken away by hand (counted from 1), each an
    // undo step; and its slices written as notes into a new clip on its track at the play start, as an undo step.
    // setSampleFile() returns false for a file that isn't a sound.
    juce::String getSampleName() const;
    bool hasSampleFile() const;
    int getFactorySample() const;
    bool setSampleFile (const juce::File&) const;
    void setFactorySample (int index) const;
    void findRootKey() const;
    std::shared_ptr<const devices::Sampler::Sound> getSampleSound() const; // for drawing it; nullptr for another device
    devices::Sampler::ManualSlices getManualSlices() const;
    std::vector<float> getSlicePositions() const;
    void moveSlice (int slice, float position) const;
    void addSlice (float position) const;
    void removeSlice (int slice) const;
    void writeSliceNotes() const;

    // What goes into the device and comes out, for its editor's displays (devices/core/DeviceScope.h); nullptr once the
    // device is gone. Ask each time: never keep the pointer.
    devices::DeviceScope* getScope() const;

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
