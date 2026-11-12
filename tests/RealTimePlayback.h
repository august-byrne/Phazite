// SPDX-License-Identifier: AGPL-3.0-or-later
// Playing in real time through the tests' audio device (TestAudioDevice.h), for the tests that need an audio device with
// inputs, which the device's own thread plays: a track's input, and a recording's correction for the inputs' delay
// (AudioRecordingTests.cpp).
#pragma once

#include "TestAudioDevice.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/core/AudioThreads.h"
#include "model/engine/EngineContext.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

namespace phazite::test
{
// Plays through the tests' audio device for as long as it lives, on a thread that acts as the audio thread, at an audio
// device's priority, and processes a block every block's length, so that what is played or moved meanwhile is recorded as
// the app records it. Its two inputs are open, so arming a track to record needn't open them again.
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

        audioThread = std::make_unique<AudioThread> (
            [this, input] (const juce::Thread& thread)
            {
                juce::AudioBuffer<float> buffer (2, blockSize);
                const auto blockDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                    std::chrono::duration<double> (blockSize / sampleRate));
                auto nextBlock = std::chrono::steady_clock::now();
                buffer.clear(); // what comes in first

                while (! thread.threadShouldExit())
                {
                    nextBlock += blockDuration;

                    if (input == Input::silence)
                        buffer.clear(); // otherwise what it played last comes back in

                    const auto called = std::chrono::steady_clock::now();
                    device->process (buffer);
                    const auto took = (std::chrono::steady_clock::now() - called).count();

                    if (took > longestCall.load (std::memory_order_relaxed))
                        longestCall.store (took, std::memory_order_relaxed);

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
        audioThread = nullptr;
        TestAudioDeviceType::close (getEngineContext().getAudioDeviceManager()); // so later tests start without it
    }

    // The longest the device's call took to play a block since this was last asked, in seconds: the engine's work, and
    // whatever the call waited on (a lock another thread held). Message thread.
    double takeLongestCall() noexcept
    {
        const std::chrono::steady_clock::duration longest (longestCall.exchange (0, std::memory_order_relaxed));
        return std::chrono::duration<double> (longest).count();
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

    // The device's thread, at the priority Phazite's own audio threads have (devices/core/AudioThreads.h), as an audio
    // device's own is: at the normal priority, a busy machine's other programs keep it from a core for tens of
    // milliseconds.
    class AudioThread final : public juce::Thread
    {
    public:
        explicit AudioThread (std::function<void (const juce::Thread&)> body)
            : juce::Thread ("Phazite test device"),
              play (std::move (body))
        {
            devices::startAudioThread (*this);
        }

        ~AudioThread() override
        {
            signalThreadShouldExit();
            stopThread (-1); // a block's length at most
        }

    private:
        std::function<void (const juce::Thread&)> play;

        void run() override
        {
            const devices::ProAudioTask proAudio;
            play (*this);
        }
    };

    juce::AudioBuffer<float> captured; // made at once, so the audio thread never allocates
    TestAudioDevice* device;
    std::atomic<int> capturedSamples { 0 };
    std::atomic<bool> capturing { false };
    std::atomic<std::chrono::steady_clock::rep> longestCall { 0 };
    std::unique_ptr<AudioThread> audioThread;
};
} // namespace phazite::test
