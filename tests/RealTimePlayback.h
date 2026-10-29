// SPDX-License-Identifier: AGPL-3.0-or-later
// Playing in real time for tests that need the song to play as the app plays it: recording MIDI (RecordingTests.cpp) and
// audio (AudioRecordingTests.cpp), and writing automation (AutomationModelTests.cpp).
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
    // What comes into the device's two inputs: silence, or what it played a block before, as a cable from an audio
    // interface's outputs to its inputs would bring it back (for recording audio: AudioRecordingTests.cpp).
    enum class Input
    {
        silence,
        loopback
    };

    explicit RealTimePlayback (te::Engine& e, Input input = Input::silence)
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

        // Tracktion adds the hosted device's MIDI inputs and outputs in messages of its own, to lists its audio callback
        // reads without a lock: the audio thread starts once they are there.
        wait (settleMilliseconds);

        audioThread = std::thread (
            [this, &audioIO, input]
            {
                juce::AudioBuffer<float> buffer (2, blockSize);
                juce::MidiBuffer midi;
                const auto blockDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                    std::chrono::duration<double> (blockSize / sampleRate));
                auto nextBlock = std::chrono::steady_clock::now();
                buffer.clear(); // what comes in first

                while (! stopping.load())
                {
                    nextBlock += blockDuration;

                    if (input == Input::silence)
                        buffer.clear(); // otherwise what it played last comes back in
                    midi.clear();
                    audioIO.processBlock (buffer, midi);
                    std::this_thread::sleep_until (nextBlock);
                }
            });

        // Tracktion hears of the device in messages of its own, after another test's playback closed its device, and
        // rebuilds an Edit's playback then: a transport started before that stops again.
        wait (settleMilliseconds);
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
    static constexpr int settleMilliseconds = 200;

    te::Engine& engine;
    std::atomic<bool> stopping { false };
    std::thread audioThread;
};
} // namespace phazite::test
