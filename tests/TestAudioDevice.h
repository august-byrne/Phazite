// SPDX-License-Identifier: AGPL-3.0-or-later
// An audio device for tests, of a type of its own on JUCE's device manager ("Phazite Test Device"): opened as the app opens
// a device, its blocks played when a test asks, on the thread that asks, with what comes into its inputs given. For
// HostedDevice.h, RealTimePlayback.h and the real-time audit.
#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <algorithm>
#include <atomic>
#include <optional>

namespace phazite::test
{
class TestAudioDevice final : public juce::AudioIODevice
{
public:
    static constexpr const char* typeName = "Phazite Test Device";
    static constexpr int numChannels = 2; // two inputs, two outputs

    TestAudioDevice();
    ~TestAudioDevice() override;

    // The device of the device manager, which must have it open (TestAudioDeviceType::open()), or nullptr.
    static TestAudioDevice* findIn (juce::AudioDeviceManager&);

    // Plays a block, on the calling thread, as the device's own thread would: what comes in is the buffer's (two
    // channels), and what goes out takes its place. Nothing plays while the device is stopped.
    void process (juce::AudioBuffer<float>&);

    juce::StringArray getOutputChannelNames() override { return { "Out 1", "Out 2" }; }
    juce::StringArray getInputChannelNames() override { return { "In 1", "In 2" }; }
    std::optional<juce::BigInteger> getDefaultOutputChannels() const override { return juce::BigInteger (3); }
    std::optional<juce::BigInteger> getDefaultInputChannels() const override { return juce::BigInteger (0); }
    juce::Array<double> getAvailableSampleRates() override { return { 44100.0, 48000.0 }; }
    juce::Array<int> getAvailableBufferSizes() override { return { 128, 256, 480, 512 }; }
    int getDefaultBufferSize() override { return 480; }

    juce::String open (const juce::BigInteger& inputChannels, const juce::BigInteger& outputChannels, double sampleRate,
                       int bufferSizeSamples) override;
    void close() override;
    bool isOpen() override { return opened; }
    void start (juce::AudioIODeviceCallback*) override;
    void stop() override;
    bool isPlaying() override { return callback.load() != nullptr; }
    juce::String getLastError() override { return {}; }

    int getCurrentBufferSizeSamples() override { return bufferSize; }
    double getCurrentSampleRate() override { return sampleRate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return outputs; }
    juce::BigInteger getActiveInputChannels() const override { return inputs; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return inputLatency; }

    // How late what comes in is, as its driver would report it (for recording through a loopback).
    void setInputLatency (int samples) noexcept { inputLatency = samples; }

private:
    bool opened = false;
    double sampleRate = 48000.0;
    int bufferSize = 480, inputLatency = 0;
    juce::BigInteger inputs, outputs;
    juce::AudioBuffer<float> incoming; // what comes in, copied before what goes out takes its place
    std::atomic<juce::AudioIODeviceCallback*> callback { nullptr };
    juce::CriticalSection lock; // between the thread that plays blocks and start() and stop(), as a driver's own
};

class TestAudioDeviceType final : public juce::AudioIODeviceType
{
public:
    TestAudioDeviceType();

    // Adds the type to a device manager (once) and opens a device of it anew with the setup, reporting so late an input as
    // its driver would (a loopback's block); what goes out of every callback then comes from process(). Returns the device,
    // or nullptr if it didn't open. close() opens no device in its place.
    static TestAudioDevice* open (juce::AudioDeviceManager&, double sampleRate, int blockSize, int numInputs = 0, int inputDelay = 0);
    static void close (juce::AudioDeviceManager&);

    void scanForDevices() override {}
    juce::StringArray getDeviceNames (bool) const override { return { "Test" }; }
    int getDefaultDeviceIndex (bool) const override { return 0; }
    int getIndexOfDevice (juce::AudioIODevice* device, bool) const override { return device != nullptr ? 0 : -1; }
    bool hasSeparateInputsAndOutputs() const override { return false; }
    juce::AudioIODevice* createDevice (const juce::String& outputDeviceName, const juce::String& inputDeviceName) override;

private:
    int inputLatency = 0; // the next device's
};
} // namespace phazite::test
