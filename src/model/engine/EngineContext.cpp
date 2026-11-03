// SPDX-License-Identifier: AGPL-3.0-or-later
#include "EngineContext.h"
#include "AudioDevices.h"
#include "internal/AudioIO.h"
#include "internal/EngineContext.h"
#include "internal/EngineSetup.h"
#include "model/engine/AudioFiles.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/plugins/internal/PluginHost.h"

namespace phazite
{
struct EngineContext::Impl
{
    te::Engine& engine;
    std::shared_ptr<AudioIO> audio = AudioIO::getFor (engine);         // the audio device and MIDI inputs (T4): the Engine's one
    PlaybackHost playback { engine, audio->getDevices() };             // Phazite's own engine (docs/tracktion-migration.md, T2)
    std::shared_ptr<PluginHost> plugins = PluginHost::getFor (engine); // plug-ins in helper processes (T3): the Engine's one

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

PlaybackHost& getPlaybackHost (EngineContext& context)
{
    return context.getImpl().playback;
}

AudioIO& getAudioIO (EngineContext& context)
{
    return *context.getImpl().audio;
}

PluginHost& getPluginHost (EngineContext& context)
{
    return *context.getImpl().plugins;
}

EngineContext::EngineContext (std::unique_ptr<Impl> i)
    : impl (std::move (i))
{
}

EngineContext::~EngineContext() = default;

juce::AudioDeviceManager& EngineContext::getAudioDeviceManager()
{
    return impl->audio->getDevices();
}

double EngineContext::getCpuUsage() const
{
    return impl->playback.getEngine().getLoad();
}

EngineContext::AudioOutput EngineContext::getAudioOutput() const
{
    auto& devices = impl->audio->getDevices();
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

double EngineContext::getRecordingCorrection() const
{
    return impl->audio->getRecordingCorrection();
}

void EngineContext::setRecordingCorrection (double milliseconds)
{
    impl->audio->setRecordingCorrection (milliseconds);
}

bool EngineContext::hasAsioDrivers() const
{
    for (auto* type : impl->audio->getDevices().getAvailableDeviceTypes())
        if (type->getTypeName() == "ASIO")
            return ! type->getDeviceNames().isEmpty();

    return false;
}

std::vector<EngineContext::MidiInput> EngineContext::getMidiInputs() const
{
    std::vector<MidiInput> inputs;

    for (const auto& input : impl->audio->getMidiInputs())
        inputs.push_back ({ input.id, input.name, input.isOn });

    return inputs;
}

void EngineContext::setMidiInputOn (const juce::String& id, bool on)
{
    impl->audio->setMidiInputOn (id, on);
}
juce::StringArray EngineContext::getAudioFileExtensions() const
{
    juce::StringArray extensions;
    auto& formats = getAudioFormats();

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
    impl->audio->addChangeListener (listener);
}

void EngineContext::removeChangeListener (juce::ChangeListener* listener)
{
    impl->audio->removeChangeListener (listener);
}
} // namespace phazite
