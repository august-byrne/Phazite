// SPDX-License-Identifier: AGPL-3.0-or-later
#include "EngineContext.h"
#include "AudioDevices.h"
#include "internal/AudioIO.h"
#include "internal/EngineContext.h"
#include "internal/PluginScanner.h"
#include "model/engine/AudioFiles.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/plugins/internal/PluginHost.h"

namespace phazite
{
struct EngineContext::Impl
{
    explicit Impl (int numThreads)
        : playback (audio, plugins, numThreads)
    {
        usePluginScannerWithTimeLimit (plugins); // scanned in Phazite's helper program, with a time limit for each
    }

    AudioIO audio;         // the audio device and MIDI inputs
    PluginHost plugins;    // plug-ins in helper processes
    PlaybackHost playback; // Phazite's engine on the device, with the previews' beside it

    // Whether the output last asked about is Bluetooth, which takes a look through Windows' devices to find out.
    juce::String checkedOutput {};
    bool checkedIsBluetooth = false;
};

std::unique_ptr<EngineContext> createEngineContext (EngineOptions options)
{
    JUCE_ASSERT_MESSAGE_THREAD

    // Not one thread per logical CPU: the second thread of a core adds little to audio, and the window and the system need a
    // core of their own (docs/archive/performance-audit.md, A2).
    const auto threads = options.numAudioThreads > 0 ? options.numAudioThreads : std::max (1, juce::SystemStats::getNumPhysicalCpus() - 1);
    return std::make_unique<EngineContext> (std::make_unique<EngineContext::Impl> (threads));
}

PlaybackHost& getPlaybackHost (EngineContext& context)
{
    return context.getImpl().playback;
}

AudioIO& getAudioIO (EngineContext& context)
{
    return context.getImpl().audio;
}

PluginHost& getPluginHost (EngineContext& context)
{
    return context.getImpl().plugins;
}

EngineContext::EngineContext (std::unique_ptr<Impl> i)
    : impl (std::move (i))
{
}

EngineContext::~EngineContext() = default;

juce::AudioDeviceManager& EngineContext::getAudioDeviceManager()
{
    return impl->audio.getDevices();
}

double EngineContext::getCpuUsage() const
{
    return impl->playback.getEngine().getLoad();
}

EngineContext::AudioOutput EngineContext::getAudioOutput() const
{
    auto& devices = impl->audio.getDevices();
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
    return impl->playback.isLowLatency();
}

double EngineContext::getRecordingCorrection() const
{
    return impl->audio.getRecordingCorrection();
}

void EngineContext::setRecordingCorrection (double milliseconds)
{
    impl->audio.setRecordingCorrection (milliseconds);
}

bool EngineContext::hasAsioDrivers() const
{
    for (auto* type : impl->audio.getDevices().getAvailableDeviceTypes())
        if (type->getTypeName() == "ASIO")
            return ! type->getDeviceNames().isEmpty();

    return false;
}

namespace
{
    // A device's channels two by two, as JUCE's device settings name them: "Out 1 + 2", from "Out 1" and "Out 2" (what both
    // names start with written once, up to a space); a last one alone as it is.
    juce::StringArray getChannelPairs (const juce::StringArray& names)
    {
        juce::StringArray pairs;

        for (int i = 0; i < names.size(); i += 2)
        {
            if (i + 1 >= names.size())
            {
                pairs.add (names[i].trim());
                continue;
            }

            const auto& first = names[i];
            const auto& second = names[i + 1];
            juce::String common;

            for (int length = 0; length <= first.length(); ++length)
                if (first.substring (0, length).equalsIgnoreCase (second.substring (0, length)))
                    common = first.substring (0, length);

            while (common.isNotEmpty() && ! juce::CharacterFunctions::isWhitespace (common.getLastCharacter()))
                common = common.dropLastCharacters (1);

            pairs.add (first.trim() + " + " + second.substring (common.length()).trim());
        }

        return pairs;
    }

    juce::Array<bool> getPairsOn (const juce::BigInteger& channels, int numPairs)
    {
        juce::Array<bool> on;

        for (int pair = 0; pair < numPairs; ++pair)
            on.add (channels[pair * 2] || channels[pair * 2 + 1]);

        return on;
    }

    bool isOpen (juce::AudioDeviceManager& devices)
    {
        auto* device = devices.getCurrentAudioDevice();
        return device != nullptr && device->isOpen();
    }

    // Opens the setup as the one chosen, or, where it doesn't open, the one before again: why it didn't. What the manager
    // keeps as the user's choice (and the settings save) changes only when a setup opens: going back isn't a choice, and
    // where nothing was open there is nothing to go back to, so that the last device that did open is tried again at the
    // next start (a setup without a device, kept as chosen, would start Phazite silent every time).
    juce::String openOrGoBack (juce::AudioDeviceManager& devices, const juce::AudioDeviceManager::AudioDeviceSetup& wanted)
    {
        const auto previous = devices.getAudioDeviceSetup();
        const auto wasOpen = isOpen (devices);
        auto error = devices.setAudioDeviceSetup (wanted, true);

        if (error.isEmpty() && isOpen (devices))
            return {};

        if (error.isEmpty())
            error = "The device didn't open.";

        if (wasOpen && (devices.setAudioDeviceSetup (previous, false).isNotEmpty() || ! isOpen (devices)))
            error += " And the device that played before didn't open again.";

        return error;
    }
} // namespace

EngineContext::AudioChoices EngineContext::getAudioChoices() const
{
    auto& devices = impl->audio.getDevices();
    AudioChoices choices;

    for (auto* type : devices.getAvailableDeviceTypes())
        choices.drivers.add (type->getTypeName());

    choices.driver = devices.getCurrentAudioDeviceType();

    if (auto* type = devices.getCurrentDeviceTypeObject())
    {
        choices.outputs = type->getDeviceNames (false);
        choices.inputsApart = type->hasSeparateInputsAndOutputs();

        if (choices.inputsApart)
            choices.inputs = type->getDeviceNames (true);
    }

    const auto setup = devices.getAudioDeviceSetup();
    choices.output = setup.outputDeviceName;
    choices.input = choices.inputsApart ? setup.inputDeviceName : juce::String();

    if (auto* device = devices.getCurrentAudioDevice(); device != nullptr && device->isOpen())
    {
        choices.isOpen = true;
        choices.sampleRates = device->getAvailableSampleRates();
        choices.sampleRate = device->getCurrentSampleRate();
        choices.bufferSizes = device->getAvailableBufferSizes();
        choices.bufferSize = device->getCurrentBufferSizeSamples();
        choices.outputPairs = getChannelPairs (device->getOutputChannelNames());
        choices.inputPairs = getChannelPairs (device->getInputChannelNames());
        choices.outputPairsOn = getPairsOn (device->getActiveOutputChannels(), choices.outputPairs.size());
        choices.inputPairsOn = getPairsOn (device->getActiveInputChannels(), choices.inputPairs.size());
        choices.hasControlPanel = device->hasControlPanel();
    }

    return choices;
}

juce::String EngineContext::chooseAudioDriver (const juce::String& driver)
{
    auto& devices = impl->audio.getDevices();
    const auto previousDriver = devices.getCurrentAudioDeviceType();

    if (driver == previousDriver)
        return {};

    const auto previous = devices.getAudioDeviceSetup();
    devices.setCurrentAudioDeviceType (driver, true);

    if (isOpen (devices))
        return {};

    devices.setCurrentAudioDeviceType (previousDriver, true);
    devices.setAudioDeviceSetup (previous, true);
    return "No device of " + driver + " opened.";
}

juce::String EngineContext::chooseAudioOutput (const juce::String& device)
{
    auto& devices = impl->audio.getDevices();
    auto setup = devices.getAudioDeviceSetup();
    setup.outputDeviceName = device;
    setup.useDefaultOutputChannels = true;

    if (auto* type = devices.getCurrentDeviceTypeObject(); type != nullptr && ! type->hasSeparateInputsAndOutputs())
    {
        setup.inputDeviceName = device;
        setup.useDefaultInputChannels = true;
    }

    return openOrGoBack (devices, setup);
}

juce::String EngineContext::chooseAudioInput (const juce::String& device)
{
    auto& devices = impl->audio.getDevices();
    auto setup = devices.getAudioDeviceSetup();
    setup.inputDeviceName = device;
    setup.useDefaultInputChannels = true;
    return openOrGoBack (devices, setup);
}

juce::String EngineContext::chooseSampleRate (double rate)
{
    auto& devices = impl->audio.getDevices();
    auto setup = devices.getAudioDeviceSetup();
    setup.sampleRate = rate;
    return openOrGoBack (devices, setup);
}

juce::String EngineContext::chooseBufferSize (int samples)
{
    auto& devices = impl->audio.getDevices();
    auto setup = devices.getAudioDeviceSetup();
    setup.bufferSize = samples;
    return openOrGoBack (devices, setup);
}

juce::String EngineContext::setChannelPairOn (bool input, int pair, bool on)
{
    auto& devices = impl->audio.getDevices();
    auto setup = devices.getAudioDeviceSetup();
    auto& channels = input ? setup.inputChannels : setup.outputChannels;

    if (pair < 0 || (channels[pair * 2] || channels[pair * 2 + 1]) == on)
        return {};

    channels.setRange (pair * 2, 2, on);

    if (! input && channels.isZero())
        return "At least one output must be on.";

    // An input's channels are the input device's: under a driver whose devices are both (ASIO), the output's.
    if (input && setup.inputDeviceName.isEmpty())
    {
        if (auto* type = devices.getCurrentDeviceTypeObject(); type == nullptr || type->hasSeparateInputsAndOutputs())
            return "Choose an input device first.";

        setup.inputDeviceName = setup.outputDeviceName;
    }

    (input ? setup.useDefaultInputChannels : setup.useDefaultOutputChannels) = false;
    return openOrGoBack (devices, setup);
}

void EngineContext::playTestSound()
{
    impl->audio.getDevices().playTestSound();
}

bool EngineContext::showDriverControlPanel()
{
    auto& devices = impl->audio.getDevices();
    auto* device = devices.getCurrentAudioDevice();

    if (device == nullptr || ! device->hasControlPanel() || ! device->showControlPanel())
        return false;

    // As JUCE's device settings do after the window: the device opened again, with what its driver says now.
    devices.closeAudioDevice();
    devices.restartLastAudioDevice();
    return true;
}

std::vector<EngineContext::MidiInput> EngineContext::getMidiInputs() const
{
    std::vector<MidiInput> inputs;

    for (const auto& input : impl->audio.getMidiInputs())
        inputs.push_back ({ input.id, input.name, input.isOn });

    return inputs;
}

void EngineContext::setMidiInputOn (const juce::String& id, bool on)
{
    impl->audio.setMidiInputOn (id, on);
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
    return phazite::getDeviceTypes (impl->plugins);
}

void EngineContext::addChangeListener (juce::ChangeListener* listener)
{
    impl->audio.addChangeListener (listener);
}

void EngineContext::removeChangeListener (juce::ChangeListener* listener)
{
    impl->audio.removeChangeListener (listener);
}
} // namespace phazite
