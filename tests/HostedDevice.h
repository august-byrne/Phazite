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
#include "model/engine/internal/EngineContext.h"
#include "model/playback/internal/PlaybackHost.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
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

    // A live note played by playLiveNote(), by the engine's clock (engine::Engine::getClock()), in samples: the start of
    // the block it was played during (just after the device's call for it returned, as a MIDI driver's thread plays one
    // between blocks), where in that block the engine took it to have come (Engine::takePlayed()'s clock), and where it
    // first sounded above -60 dBFS; each -1 if it didn't. Phazite's engine plays a live note in the next block, as far into
    // it as it came after the block before started by the wall clock (engine::Engine::playLive()), on a thread that a
    // busy machine may hold up anywhere: so the note's sound is measured from where the engine plays it, by the engine's
    // clock alone, which no load moves; and where it took the note to have come is checked against the wall clock read on
    // both sides of the block and of the note, which always holds it, however long the threads were held up.
    struct LiveNote
    {
        std::int64_t blockStart = -1, cameAt = -1, sounded = -1;
        int earliest = 0, latest = 0; // the samples into the block that the note came at by the wall clock, at the least and most

        // Where the engine plays the note: a block after where it came, the same for every note.
        std::int64_t getPlayedAt() const noexcept { return cameAt >= 0 ? cameAt + blockSize : -1; }

        // The samples from where the engine plays the note to its sound: the delay of what plays it (delay compensation, a
        // device's look-ahead), or -1 if it went unheard or the engine didn't take it.
        int toSound() const noexcept { return cameAt >= 0 && sounded >= getPlayedAt() ? (int) (sounded - getPlayedAt()) : -1; }

        // Whether the engine took the note in the block after it came, as far into it as it came by the wall clock.
        bool isPlacedAsPlayed() const noexcept { return cameAt >= blockStart + earliest && cameAt <= blockStart + latest; }
    };

    // Plays a note on the device's thread just after a block, as a MIDI driver's thread would, and the blocks after it
    // until it sounds (40 at most). pitch is the note playNote() plays, by which the engine's place for it is found.
    LiveNote playLiveNote (const std::function<void()>& playNote, int pitch)
    {
        auto& engine = getPlaybackHost (getEngineContext()).getEngine();
        LiveNote note;
        std::thread audioThread (
            [&]
            {
                juce::AudioBuffer<float> buffer (2, blockSize);
                buffer.clear();
                note.blockStart = engine.getClock();
                const auto blockStarted = phazite::devices::Clock::getMilliseconds();
                device->process (buffer); // the engine reads the wall clock somewhere in here
                const auto blockEnded = phazite::devices::Clock::getMilliseconds();
                const auto before = phazite::devices::Clock::getMilliseconds();
                playNote();
                const auto after = phazite::devices::Clock::getMilliseconds();

                // The engine's block started between blockStarted and blockEnded, the note came between before and after
                // (and a sample more each way, for the engine's rounding).
                const auto toSamples = [] (double milliseconds) { return milliseconds * sampleRate / 1000.0; };
                note.earliest = std::clamp ((int) std::floor (toSamples (before - blockEnded)) - 1, 0, blockSize - 1);
                note.latest = std::clamp ((int) std::ceil (toSamples (after - blockStarted)) + 1, 0, blockSize - 1);

                for (int b = 0; b < 40 && note.sounded < 0; ++b)
                {
                    buffer.clear();
                    device->process (buffer);

                    for (int i = 0; i < blockSize && note.sounded < 0; ++i)
                        if (std::abs (buffer.getSample (0, i)) > 0.001f || std::abs (buffer.getSample (1, i)) > 0.001f)
                            note.sounded = note.blockStart + (b + 1) * blockSize + i;
                }
            });
        audioThread.join();

        for (const auto& played : getPlaybackHost (getEngineContext()).getPlayedEvents())
            if (played.clock >= note.blockStart && played.event.number == pitch && played.event.type == devices::NoteEvent::Type::noteOn)
            {
                note.cameAt = played.clock;
                break;
            }

        return note;
    }

private:
    TestAudioDevice* device;
};
} // namespace phazite::test
