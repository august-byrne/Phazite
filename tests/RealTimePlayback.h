// SPDX-License-Identifier: AGPL-3.0-or-later
// Playing in real time for tests that need the song to play as the app plays it: recording MIDI (RecordingTests.cpp) and
// writing automation (AutomationModelTests.cpp).
#pragma once

#include "UiTestHelpers.h"
#include "model/base/internal/Tracktion.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace phazite::test
{
// Plays an Edit through Tracktion's hosted audio device for as long as it lives, on a thread that acts as the audio
// thread and processes a block every block's length, so that what is played or moved meanwhile is recorded as the app
// records it.
class RealTimePlayback
{
public:
    explicit RealTimePlayback (te::Engine& e)
        : engine (e)
    {
        auto& deviceManager = engine.getDeviceManager();
        auto& audioIO = deviceManager.getHostedAudioDeviceInterface();

        te::HostedAudioDeviceInterface::Parameters params;
        params.sampleRate = sampleRate;
        params.blockSize = blockSize;
        audioIO.initialise (params);
        audioIO.prepareToPlay (sampleRate, blockSize);
        deviceManager.dispatchPendingUpdates();

        // Tracktion hears of the device in messages of its own, after another test's playback closed its device, and
        // rebuilds an Edit's playback then: a transport started before it stops again.
        dispatchPendingMessages();

        audioThread = std::thread (
            [this, &audioIO]
            {
                juce::AudioBuffer<float> buffer (2, blockSize);
                juce::MidiBuffer midi;
                const auto blockDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                    std::chrono::duration<double> (blockSize / sampleRate));
                auto nextBlock = std::chrono::steady_clock::now();

                while (! stopping.load())
                {
                    nextBlock += blockDuration;
                    buffer.clear();
                    midi.clear();
                    audioIO.processBlock (buffer, midi);
                    std::this_thread::sleep_until (nextBlock);
                }
            });
    }

    ~RealTimePlayback()
    {
        stopping = true;
        audioThread.join();

        // As Tracktion's own tests do, so later tests start without the hosted device.
        auto& deviceManager = engine.getDeviceManager();
        deviceManager.closeDevices();
        deviceManager.removeHostedAudioDeviceInterface();
        deviceManager.deviceManager.closeAudioDevice();
    }

    // Lets some time pass on the message thread, which delivers its messages and runs its timers meanwhile.
    static void wait (int milliseconds)
    {
        dispatchMessagesUntil ([] { return false; }, milliseconds);
    }

private:
    static constexpr double sampleRate = 48000.0;
    static constexpr int blockSize = 256;

    te::Engine& engine;
    std::atomic<bool> stopping { false };
    std::thread audioThread;
};
} // namespace phazite::test
