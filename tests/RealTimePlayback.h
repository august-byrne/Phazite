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
// records it. Nothing may open the device again meanwhile: unlike the computer's own devices, the hosted one hands the
// thread's blocks to the callback it was started with, which it sets and reads unsynchronised (docs/realtime-audit.md,
// finding 24). Its inputs are all open, so arming a track to record needn't open it again.
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

    // With captureSeconds, it can keep that much of what it plays (startCapture()).
    explicit RealTimePlayback (te::Engine& e, Input input = Input::silence, double captureSeconds = 0.0)
        : engine (e),
          captured (2, (int) (captureSeconds * sampleRate))
    {
        auto& deviceManager = engine.getDeviceManager();
        auto& audioIO = deviceManager.getHostedAudioDeviceInterface();

        // As Tracktion's own test player does (tracktion_EnginePlayer.h): after a block that took longer to process than
        // it lasts, as blocks can in a slow build (ThreadSanitizer's), Tracktion skips the next block, as when the computer
        // can't keep up: what comes in then is lost, and nothing plays. These tests play as if it always kept up.
        deviceManager.setCpuLimitBeforeMuting (1000.0);

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

                    if (capturing.load (std::memory_order_acquire))
                    {
                        const auto at = capturedSamples.load (std::memory_order_relaxed);
                        const auto length = std::min (blockSize, captured.getNumSamples() - at);

                        for (int channel = 0; channel < captured.getNumChannels() && length > 0; ++channel)
                            captured.copyFrom (channel, at, buffer, channel, 0, length);

                        capturedSamples.store (at + length, std::memory_order_release);
                    }

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
        deviceManager.setCpuLimitBeforeMuting (0.98); // Tracktion's own again
    }

    // Lets some time pass on the message thread, which delivers its messages and runs its timers meanwhile.
    static void wait (int milliseconds)
    {
        dispatchMessagesUntil ([] { return false; }, milliseconds);
    }

    // Keeps what it plays from the next block on, in stereo, until stopCapture() or the captureSeconds it was made with,
    // which returns it. Message thread.
    void startCapture()
    {
        capturedSamples.store (0, std::memory_order_relaxed);
        capturing.store (true, std::memory_order_release);
    }

    juce::AudioBuffer<float> stopCapture()
    {
        capturing.store (false, std::memory_order_release);
        const auto length = capturedSamples.load (std::memory_order_acquire); // what the audio thread wrote and won't touch again
        juce::AudioBuffer<float> audio (captured.getNumChannels(), length);

        for (int channel = 0; channel < audio.getNumChannels(); ++channel)
            audio.copyFrom (channel, 0, captured, channel, 0, length);

        return audio;
    }

    static constexpr double sampleRate = 48000.0;

private:
    static constexpr int blockSize = 256;
    static constexpr int settleMilliseconds = 200;

    te::Engine& engine;
    juce::AudioBuffer<float> captured; // made at once, so the audio thread never allocates
    std::atomic<int> capturedSamples { 0 };
    std::atomic<bool> capturing { false }, stopping { false };
    std::thread audioThread;
};
} // namespace phazite::test
