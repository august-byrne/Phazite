// SPDX-License-Identifier: AGPL-3.0-or-later
// The tests' audio device (TestAudioDevice.h), opened on Phazite's device manager as the app opens a device, for tests and
// benchmarks that process audio blocks themselves, on a thread of their own as the audio thread would, one call at a time:
// to find when a live note sounds, or to time what plays. (Tests that need playback to run on its own in real time use
// RealTimePlayback.h.)
#pragma once

#include "GoldenFiles.h"
#include "TestAudioDevice.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "model/engine/EngineContext.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <thread>

namespace phazite::test
{
// The engine's audio comes from this device for as long as it lives; it closes again after, so that later tests start
// without it.
class HostedDevice
{
public:
    static constexpr double sampleRate = 48000.0;
    static constexpr int blockSize = 480; // 10 ms, the owner's setting in October 2026

    HostedDevice()
        : device (TestAudioDeviceType::open (getEngineContext().getAudioDeviceManager(), sampleRate, blockSize))
    {
        REQUIRE (device != nullptr);
        dispatchPendingMessages(); // the song for the device's rate
    }

    ~HostedDevice()
    {
        TestAudioDeviceType::close (getEngineContext().getAudioDeviceManager());
        dispatchPendingMessages();
    }

    // Processes some blocks on another thread, and waits for it: each one, if given, sees each block's output and can stop
    // early by returning true. paceMicroseconds pauses between blocks.
    void process (int count, const std::function<bool (const juce::AudioBuffer<float>&, int)>& each = {}, int paceMicroseconds = 500)
    {
        std::thread audioThread (
            [&]
            {
                juce::AudioBuffer<float> buffer (2, blockSize);

                for (int b = 0; b < count; ++b)
                {
                    buffer.clear();
                    device->process (buffer);

                    if (each && each (buffer, b))
                        break;

                    if (paceMicroseconds > 0)
                        std::this_thread::sleep_for (std::chrono::microseconds (paceMicroseconds));
                }
            });
        audioThread.join();
    }

    // How long a block takes to process over some blocks processed back to back, as a percentage of their audio's length.
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

    // Plays a note on the device's thread just after a block, as a MIDI driver's thread would, and says how many samples
    // after it was played it sounds above -60 dBFS, or -1 if it doesn't within 40 blocks. Phazite's engine plays a live
    // note as far into the next block as it came after the block before started (engine::Engine::playLive()), which this
    // times as the engine does: what is left is the delay of what plays it (delay compensation, a device's look-ahead).
    int samplesFromNoteToSound (const std::function<void()>& playNote)
    {
        int found = -1;
        std::thread audioThread (
            [&]
            {
                juce::AudioBuffer<float> buffer (2, blockSize);
                buffer.clear();
                const auto blockStarted = juce::Time::getMillisecondCounterHiRes();
                device->process (buffer);
                const auto before = juce::Time::getMillisecondCounterHiRes();
                playNote();
                const auto after = juce::Time::getMillisecondCounterHiRes();
                const auto played = (before + after) / 2.0;
                const auto intoBlock = std::clamp (juce::roundToInt ((played - blockStarted) * sampleRate / 1000.0), 0, blockSize - 1);
                uncertainty = (int) std::ceil ((after - before) * sampleRate / 2000.0);

                for (int b = 0; b < 40 && found < 0; ++b)
                {
                    buffer.clear();
                    device->process (buffer);

                    for (int i = 0; i < blockSize && found < 0; ++i)
                        if (std::abs (buffer.getSample (0, i)) > 0.001f || std::abs (buffer.getSample (1, i)) > 0.001f)
                            found = b * blockSize + i - intoBlock;
                }
            });
        audioThread.join();
        return found;
    }

    // How many samples the last delay may be off by: half the time playing the note took, which it was played somewhere in
    // (a sanitizer's build makes that time long, so that delays measured to the sample jitter by more than one).
    int getUncertainty() const noexcept { return uncertainty; }

private:
    TestAudioDevice* device;
    int uncertainty = 0;
};
} // namespace phazite::test
