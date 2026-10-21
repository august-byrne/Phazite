// SPDX-License-Identifier: AGPL-3.0-or-later
#include "EngineContext.h"
#include "AudioDevices.h"
#include "internal/EngineContext.h"
#include "internal/EngineSetup.h"
#include "model/mixer/internal/Mixer.h"

namespace phazite
{
struct EngineContext::Impl
{
    te::Engine& engine;

    // Whether the output last asked about is Bluetooth, which takes a look through Windows' devices to find out.
    juce::String checkedOutput {};
    bool checkedIsBluetooth = false;
};

std::unique_ptr<EngineContext> createEngineContext (te::Engine& engine)
{
    return std::make_unique<EngineContext> (std::make_unique<EngineContext::Impl> (engine));
}

te::Engine& getEngine (EngineContext& context)
{
    return context.getImpl().engine;
}

EngineContext::EngineContext (std::unique_ptr<Impl> i)
    : impl (std::move (i))
{
}

EngineContext::~EngineContext() = default;

juce::AudioDeviceManager& EngineContext::getAudioDeviceManager()
{
    return impl->engine.getDeviceManager().deviceManager;
}

double EngineContext::getCpuUsage() const
{
    return impl->engine.getDeviceManager().getCpuUsage();
}

EngineContext::AudioOutput EngineContext::getAudioOutput() const
{
    auto& devices = impl->engine.getDeviceManager().deviceManager;
    AudioOutput output;
    auto* device = devices.getCurrentAudioDevice();

    if (device == nullptr || ! device->isOpen())
        return output;

    output.isOpen = true;
    output.type = devices.getCurrentAudioDeviceType();
    output.name = devices.getAudioDeviceSetup().outputDeviceName;
    output.sampleRate = device->getCurrentSampleRate();
    output.bufferSamples = device->getCurrentBufferSizeSamples();

    if (output.sampleRate > 0.0)
    {
        output.bufferSeconds = output.bufferSamples / output.sampleRate;
        output.latencySeconds = device->getOutputLatencyInSamples() / output.sampleRate;
    }

    if (const auto key = output.type + "\n" + output.name; key != impl->checkedOutput)
    {
        impl->checkedOutput = key;
        impl->checkedIsBluetooth = isBluetoothOutput (output.type, output.name);
    }

    output.isBluetooth = impl->checkedIsBluetooth;
    return output;
}

bool EngineContext::isLowLatency() const
{
    return phazite::isLowLatency (impl->engine);
}

bool EngineContext::hasAsioDrivers() const
{
    for (auto* type : impl->engine.getDeviceManager().deviceManager.getAvailableDeviceTypes())
        if (type->getTypeName() == "ASIO")
            return ! type->getDeviceNames().isEmpty();

    return false;
}

std::vector<EngineContext::MidiInput> EngineContext::getMidiInputs() const
{
    std::vector<MidiInput> inputs;

    // Not Phazite's own inputs (the computer keyboard's, for one), which Tracktion lists too.
    for (const auto& device : impl->engine.getDeviceManager().getMidiInDevices())
        if (device->getDeviceType() == te::InputDevice::physicalMidiDevice)
            inputs.push_back ({ device->getDeviceID(), device->getName(), device->isEnabled() });

    return inputs;
}

void EngineContext::setMidiInputOn (const juce::String& id, bool on)
{
    // Tracktion may replace its device objects when devices change: found by ID each time.
    for (const auto& device : impl->engine.getDeviceManager().getMidiInDevices())
        if (device->getDeviceID() == id)
            device->setEnabled (on);
}

juce::StringArray EngineContext::getAudioFileExtensions() const
{
    juce::StringArray extensions;
    auto& formats = impl->engine.getAudioFileFormatManager().readFormatManager;

    for (int i = 0; i < formats.getNumKnownFormats(); ++i)
        for (const auto& extension : formats.getKnownFormat (i)->getFileExtensions())
            extensions.addIfNotAlreadyThere (extension.toLowerCase());

    return extensions;
}

std::vector<DeviceType> EngineContext::getDeviceTypes() const
{
    return phazite::getDeviceTypes (impl->engine);
}

void EngineContext::addChangeListener (juce::ChangeListener* listener)
{
    impl->engine.getDeviceManager().addChangeListener (listener);
}

void EngineContext::removeChangeListener (juce::ChangeListener* listener)
{
    impl->engine.getDeviceManager().removeChangeListener (listener);
}
} // namespace phazite
