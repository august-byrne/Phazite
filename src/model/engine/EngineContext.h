// SPDX-License-Identifier: AGPL-3.0-or-later
// The engine as the UI reaches it (architecture phase A3): the audio devices, how busy the engine is, and the MIDI
// inputs. The app makes the one EngineContext as it starts (src/app/Main.cpp, with internal/EngineContext.h) and hands
// it to its services (src/app/AppServices.h). Message thread only.
#pragma once

#include "model/mixer/DeviceType.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <memory>
#include <vector>

namespace phazite
{
class EngineContext
{
public:
    ~EngineContext();

    // JUCE's device manager, which the audio settings show; the app keeps what they change in Phazite's settings.
    juce::AudioDeviceManager& getAudioDeviceManager();

    // How much of the time available for each audio block the engine uses, from 0 to 1.
    double getCpuUsage() const;

    // The audio output as it plays now: its driver and device, the sample rate and buffer, and the latency its driver
    // reports (from the engine's output to the device's, the buffer included); and whether it is Bluetooth, whose own
    // delay, about 150 to 250 ms more, no driver reports (model/engine/AudioDevices.h). isOpen is false without one.
    struct AudioOutput
    {
        bool isOpen = false;
        juce::String type, name;
        double sampleRate = 0.0;
        int bufferSamples = 0;
        double bufferSeconds = 0.0, latencySeconds = 0.0;
        bool isBluetooth = false;
    };

    AudioOutput getAudioOutput() const;

    // Whether any ASIO driver is installed (Windows): an audio interface's own gives the lowest delay.
    bool hasAsioDrivers() const;

    // What the audio settings offer (ui/shell/SettingsPanel.h): the drivers (JUCE's device types: "Windows Audio", "ASIO"),
    // the devices of the one in use and those chosen, and what the device open now offers, with what it plays at. A driver
    // whose devices are each an output and an input at once (ASIO) has no inputs apart: its output names the device. The
    // channels go two by two, as stereo pairs ("Out 1 + 2"), each on or off. Without a device open, the device's part is
    // empty.
    struct AudioChoices
    {
        juce::StringArray drivers;
        juce::String driver;
        juce::StringArray outputs, inputs;
        juce::String output, input; // input empty for none
        bool inputsApart = true;
        bool isOpen = false;
        juce::Array<double> sampleRates;
        double sampleRate = 0.0;
        juce::Array<int> bufferSizes;
        int bufferSize = 0;
        juce::StringArray outputPairs, inputPairs;
        juce::Array<bool> outputPairsOn, inputPairsOn;
        bool hasControlPanel = false;
    };

    AudioChoices getAudioChoices() const;

    // Each opens the audio device so, as the settings choose it (what Phazite's settings keep from then on). Where it doesn't
    // open, what played before opens again, and the answer is why (empty when it opened). A driver opens its default
    // device; an output its default channels, and, under a driver without inputs apart, its inputs too; an input, empty for
    // none, its default channels. The last output pair can't be turned off: something must play; an input pair needs an input
    // device (under a driver without inputs apart, the output's).
    juce::String chooseAudioDriver (const juce::String& driver);
    juce::String chooseAudioOutput (const juce::String& device);
    juce::String chooseAudioInput (const juce::String& device);
    juce::String chooseSampleRate (double);
    juce::String chooseBufferSize (int samples);
    juce::String setChannelPairOn (bool input, int pair, bool on);

    // A short tone through the output, to hear that it works.
    void playTestSound();

    // The driver's own window (an ASIO driver's control panel), where the device has one: it returns once the window
    // closes, and the device opens again where the driver says something may have changed (the answer). The caller keeps
    // its own windows from input meanwhile, as the window is the driver's.
    bool showDriverControlPanel();

    // Whether the Low Latency switch is on (the command transport.lowLatency; model/playback/internal/PlaybackHost.h).
    bool isLowLatency() const;

    // The correction for recordings, in milliseconds (model/transport/internal/AudioRecorder.h): how much earlier what the
    // audio inputs record goes than the delays their driver reports put it, for a driver that reports them wrongly; 0 at
    // first, and best left so.
    double getRecordingCorrection() const;
    void setRecordingCorrection (double milliseconds);

    // The MIDI devices connected, each on or off, listed again as they are plugged in. Phazite opens those on, and what
    // they play goes to the track live MIDI plays.
    struct MidiInput
    {
        juce::String id, name;
        bool isOn = false;
    };

    std::vector<MidiInput> getMidiInputs() const;
    void setMidiInputOn (const juce::String& id, bool); // an ID that is gone changes nothing

    // The extensions of the audio files the engine reads, as ".wav"; and the devices that can be added (Mixer.h's
    // getDeviceTypes()), as the browser lists them.
    juce::StringArray getAudioFileExtensions() const;
    std::vector<DeviceType> getDeviceTypes() const;

    // Hears when audio or MIDI devices come, go or change.
    void addChangeListener (juce::ChangeListener*);
    void removeChangeListener (juce::ChangeListener*);

    struct Impl; // the audio device, the plug-ins' helpers and Phazite's engine (internal/EngineContext.h)
    Impl& getImpl() const noexcept { return *impl; }
    explicit EngineContext (std::unique_ptr<Impl>);

private:
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE (EngineContext)
};
} // namespace phazite
