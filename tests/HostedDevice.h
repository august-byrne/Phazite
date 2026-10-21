// SPDX-License-Identifier: AGPL-3.0-or-later
// Tracktion's hosted audio device for tests and benchmarks that process audio blocks themselves, on a thread of their own
// as the audio thread would, one call at a time: to find when a live note sounds, or to time the playback graph. (Tests
// that need playback to run on its own in real time use RealTimePlayback.h.)
#pragma once

#include "GoldenFiles.h"
#include "TestHelpers.h"

#include <chrono>
#include <cmath>
#include <functional>
#include <thread>

namespace phazite::test
{
// The engine's audio comes from this device for as long as it lives; it is closed again as Tracktion's own tests close
// it, so that later tests start without it.
class HostedDevice
{
public:
    static constexpr double sampleRate = 48000.0;
    static constexpr int blockSize = 480; // 10 ms, the owner's setting in October 2026

    HostedDevice()
        : audioIO (engine.getDeviceManager().getHostedAudioDeviceInterface())
    {
        te::HostedAudioDeviceInterface::Parameters parameters;
        parameters.sampleRate = sampleRate;
        parameters.blockSize = blockSize;
        audioIO.initialise (parameters);
        audioIO.prepareToPlay (sampleRate, blockSize);
        engine.getDeviceManager().dispatchPendingUpdates();

        // Tracktion adds the device's MIDI inputs and outputs in messages of its own, to lists the audio callback reads
        // without a lock: blocks are processed once they are there.
        settle (500);
    }

    ~HostedDevice()
    {
        auto& deviceManager = engine.getDeviceManager();
        deviceManager.closeDevices();
        deviceManager.removeHostedAudioDeviceInterface();
        deviceManager.deviceManager.closeAudioDevice();
        settle (300);
    }

    // Processes some blocks on another thread, and waits for it: each one, if given, sees each block's output and can stop
    // early by returning true. paceMicroseconds pauses between blocks.
    void process (int count, const std::function<bool (const juce::AudioBuffer<float>&, int)>& each = {}, int paceMicroseconds = 500)
    {
        std::thread audioThread (
            [&]
            {
                juce::AudioBuffer<float> buffer (2, blockSize);
                juce::MidiBuffer midi;

                for (int b = 0; b < count; ++b)
                {
                    buffer.clear();
                    midi.clear();
                    audioIO.processBlock (buffer, midi);

                    if (each && each (buffer, b))
                        break;

                    if (paceMicroseconds > 0)
                        std::this_thread::sleep_for (std::chrono::microseconds (paceMicroseconds));
                }
            });
        audioThread.join();
    }

    // How long processBlock() takes over some blocks processed back to back, as a percentage of their audio's length.
    double percentOfRealTime (int count)
    {
        process (20, {}, 0);
        const auto start = std::chrono::steady_clock::now();
        process (count, {}, 0);
        const auto seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
        return 100.0 * seconds / (count * blockSize / sampleRate);
    }

    // The samples from the start of the next block processed to the first sound above -60 dBFS, or -1 if none comes within
    // 40 blocks.
    int samplesToFirstSound()
    {
        int found = -1;
        process (40,
                 [&found] (const juce::AudioBuffer<float>& buffer, int b)
                 {
                     for (int i = 0; i < blockSize; ++i)
                         if (std::abs (buffer.getSample (0, i)) > 0.001f || std::abs (buffer.getSample (1, i)) > 0.001f)
                         {
                             found = b * blockSize + i;
                             return true;
                         }

                     return false;
                 });
        return found;
    }

private:
    te::Engine& engine = getEngine();
    te::HostedAudioDeviceInterface& audioIO;
};
} // namespace phazite::test
