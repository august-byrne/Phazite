// SPDX-License-Identifier: AGPL-3.0-or-later
// One of Phazite's own devices in Tracktion (docs/design.md, section 24): a thin plug-in around the device's
// engine-neutral DSP (src/devices/). It turns the device's parameters into automatable parameters, stored in its
// state under their IDs, and hands the DSP each block's audio (always stereo), notes, parameter values and tempo,
// and the sound of its sidechain's source (model/mixer/internal/Sidechain.h) for a device that has one.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"
#include "devices/dsp/DelayLine.h"
#include "model/base/internal/Tracktion.h"

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>

namespace phazite
{
class DevicePlugin : public te::Plugin
{
public:
    DevicePlugin (te::PluginCreationInfo, const devices::DeviceInfo&, std::unique_ptr<devices::Processor>);
    ~DevicePlugin() override;

    const devices::DeviceInfo& getDeviceInfo() const noexcept { return deviceInfo; }

    // The device's DSP, for the model code of a device that has more than parameters (its meters, its samples).
    devices::Processor& getProcessor() noexcept { return *processor; }

    // The parameter with that ID, or nullptr.
    te::AutomatableParameter* findParameter (std::string_view id) const;

    juce::String getName() const override;
    juce::String getPluginType() override;
    juce::String getShortName (int) override;
    juce::String getSelectableDescription() override;

    bool takesMidiInput() override;
    bool isSynth() override;
    bool producesAudioWhenNoAudioInput() override;
    bool noTail() override;
    double getTailLength() const override;
    double getLatencySeconds() override;
    int getNumOutputChannelsGivenInputs (int) override { return 2; }
    void getChannelNames (juce::StringArray* ins, juce::StringArray* outs) override;
    BusLayout getBusses() const override;

    void initialise (const te::PluginInitialisationInfo&) override;
    void initialiseWithoutStopping (const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void reset() override;
    void applyToBuffer (const te::PluginRenderContext&) override; // real-time safe: no locks, no allocation
    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

private:
    static constexpr int maximumEvents = 1024; // more in one block are dropped

    const devices::DeviceInfo& deviceInfo;
    std::unique_ptr<devices::Processor> processor;

    juce::OwnedArray<juce::CachedValue<float>> storedValues; // in the plug-in's state, under the parameters' IDs

    // In the device's order. Counted references, like Tracktion's own plug-ins hold: notifyListenersOfDeletion() makes
    // te::Plugin let go of its parameters, and the destructor detaches them after that.
    std::vector<te::AutomatableParameter::Ptr> parameters;
    std::vector<float> currentValues; // what the DSP reads, written on the audio thread
    std::array<devices::NoteEvent, maximumEvents> events {};

    juce::AudioBuffer<float> stereo; // for a mono channel: sized in initialise(), as long as the stretches played at once
    std::optional<te::tempo::Sequence::Position> tempoPosition;
    int latencySamples = 0;
    double latencySeconds = 0.0;

    // Whether a track feeds the sidechain, set as each playback graph is built: its sound then comes in as the buffer's
    // third and fourth channels, after the device's own two.
    std::atomic<bool> sidechainConnected { false };

    // An effect's mix (DeviceInfo.h, mixParameterID): what came in, delayed as much as the effect delays its sound.
    int mixIndex = -1;
    float appliedMix = 1.0f; // where the last stretch's ramp ended
    juce::AudioBuffer<float> dry;
    std::array<devices::dsp::DelayLine, 2> dryDelays;

    int collectEvents (const te::PluginRenderContext&);
    void mixWithDry (float* const* wet, int length, float targetMix);
    void updateSidechain();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DevicePlugin)
};

// The plug-in type of a device whose DSP class gives its description (Device::info) and type (Device::typeID, the same
// as info.typeID). Register it with te::PluginManager::createBuiltInType<DevicePluginOf<Device>>().
template <typename Device>
class DevicePluginOf final : public DevicePlugin
{
public:
    static inline const char* const xmlTypeName = Device::typeID;

    explicit DevicePluginOf (te::PluginCreationInfo creation)
        : DevicePlugin (creation, Device::info, std::make_unique<Device>())
    {
    }
};
} // namespace phazite
