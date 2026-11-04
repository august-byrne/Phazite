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
