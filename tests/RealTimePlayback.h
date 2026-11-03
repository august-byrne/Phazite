// SPDX-License-Identifier: AGPL-3.0-or-later
// Playing in real time through the tests' audio device (TestAudioDevice.h), for the tests that need an audio device with
// inputs, which the device's own thread plays: a track's input, and a recording's correction for the inputs' delay
// (AudioRecordingTests.cpp).
#pragma once

#include "TestAudioDevice.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "model/engine/EngineContext.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace phazite::test
{
// Plays through the tests' audio device for as long as it lives, on a thread that acts as the audio thread and processes a
// block every block's length, so that what is played or moved meanwhile is recorded as the app records it. Its two inputs
// are open, so arming a track to record needn't open them again.
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
    explicit RealTimePlayback (Input input = Input::silence, double captureSeconds = 0.0)
        : captured (2, (int) (captureSeconds * sampleRate)),
          device (TestAudioDeviceType::open (getEngineContext().getAudioDeviceManager(), sampleRate, blockSize, 2,
                                             input == Input::loopback ? blockSize : 0)) // a loopback comes back a block late
    {
        REQUIRE (device != nullptr);
        wait (settleMilliseconds); // the song for the device's rate

        audioThread = std::thread (
            [this, input]
            {
                juce::AudioBuffer<float> buffer (2, blockSize);
                const auto blockDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                    std::chrono::duration<double> (blockSize / sampleRate));
                auto nextBlock = std::chrono::steady_clock::now();
                buffer.clear(); // what comes in first

                while (! stopping.load())
                {
                    nextBlock += blockDuration;

                    if (input == Input::silence)
                        buffer.clear(); // otherwise what it played last comes back in

                    device->process (buffer);

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
    }

    ~RealTimePlayback()
    {
        stopping = true;
        audioThread.join();
        TestAudioDeviceType::close (getEngineContext().getAudioDeviceManager()); // so later tests start without it
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

    juce::AudioBuffer<float> captured; // made at once, so the audio thread never allocates
    TestAudioDevice* device;
    std::atomic<int> capturedSamples { 0 };
    std::atomic<bool> capturing { false }, stopping { false };
    std::thread audioThread;
};
} // namespace phazite::test
