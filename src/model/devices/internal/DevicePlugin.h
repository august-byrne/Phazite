// SPDX-License-Identifier: AGPL-3.0-or-later
// One of Phazite's own devices in Tracktion (docs/design.md, section 24): a thin plug-in around the device as an engine
// plays it (engine/nodes/DeviceRunner.h). It turns the device's parameters into automatable parameters, stored in its
// state under their IDs, and hands the runner each block's audio (always stereo), notes, parameter values, tempo and
// modulation, and the sound of its sidechain's source (model/mixer/internal/Sidechain.h) for a device that has one.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/DeviceScope.h"
#include "devices/core/Processor.h"
#include "engine/core/Handover.h"
#include "engine/nodes/DeviceRunner.h"
#include "model/base/internal/Tracktion.h"
#include "model/devices/internal/DeviceModulation.h"

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

    const devices::DeviceInfo& getDeviceInfo() const noexcept { return runner.getDeviceInfo(); }

    // The device's DSP, for the model code of a device that has more than parameters (its meters, its samples).
    devices::Processor& getProcessor() noexcept { return runner.getProcessor(); }

    // What goes into it and comes out, after its mix, for its editor's displays (devices/core/DeviceScope.h).
    devices::DeviceScope& getScope() noexcept { return runner.getScope(); }

    // The parameter with that ID, or nullptr.
    te::AutomatableParameter* findParameter (std::string_view id) const;

    // Gives the audio thread what modulates the device's parameters (DeviceModulation.h), which its runner then moves
    // itself in every stretch of a block; with nothing, Tracktion's modulation moves them, once a block. Message thread.
    void setModulation (std::unique_ptr<DeviceModulation>);
    const DeviceModulation* getModulation() const noexcept { return modulations.get(); } // as last given, or none

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
    engine::DeviceRunner runner;

    juce::OwnedArray<juce::CachedValue<float>> storedValues; // in the plug-in's state, under the parameters' IDs

    // In the device's order. Counted references, like Tracktion's own plug-ins hold: notifyListenersOfDeletion() makes
    // te::Plugin let go of its parameters, and the destructor detaches them after that.
    std::vector<te::AutomatableParameter::Ptr> parameters;
    std::vector<float> values; // each one's value as a block starts, for the runner: written on the audio thread

    // What modulates its parameters, handed to the audio thread whole.
    engine::Handover<DeviceModulation> modulations;
    void startModulation (DeviceModulation&, double tempo, int numerator, bool isPlaying);

    std::array<devices::NoteEvent, engine::DeviceRunner::maximumEvents> events {};
    std::optional<te::tempo::Sequence::Position> tempoPosition;
    double latencySeconds = 0.0;
    bool isBypassedForLowLatency() const;

    // Whether a track feeds the sidechain, set as each playback graph is built: its sound then comes in as the buffer's
    // third and fourth channels, after the device's own two.
    std::atomic<bool> sidechainConnected { false };

    int collectEvents (const te::PluginRenderContext&);
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
