// SPDX-License-Identifier: AGPL-3.0-or-later
// What Phazite's engine records of a take (docs/tracktion-migration.md, T2 step 9): each armed track's audio input, from
// the first sample the song plays after the take is given to the engine (after the count-in) for as long as it plays on
// without a stop or a jump, into a buffer of its own that one reading thread empties; and where in the song that first
// sample was, with the engine's clock then, so that the take's place is known to the sample. The notes played live
// meanwhile come out of the engine with the clock they came in at (Engine::takePlayed()). Made and freed on the message
// thread, handed to the engine whole (Engine::setRecording()); the audio thread writes into it, real-time safe.
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace phazite::engine
{
class Recording
{
public:
    // An input recorded: the audio device's channels it takes (one, or a pair), into a buffer of so many frames.
    class Input
    {
    public:
        Input (std::vector<int> channels, int capacity);

        const std::vector<int>& getChannels() const noexcept { return channels; }

        // The reading thread (one at a time): what has come in, up to so many frames, handed to a function that takes the
        // channels and a count of frames, in one part or two; how many frames it handed.
        template <typename Write>
        int read (int maxFrames, Write&& write)
        {
            const auto scope = fifo.read (std::min (maxFrames, fifo.getNumReady()));
            const auto part = [this, &write] (int start, int count)
            {
                if (count <= 0)
                    return;

                const float* data[] = { buffer.getReadPointer (0, start), buffer.getReadPointer (buffer.getNumChannels() - 1, start) };
                write (data, (int) channels.size(), count);
            };

            part (scope.startIndex1, scope.blockSize1);
            part (scope.startIndex2, scope.blockSize2);
            return scope.blockSize1 + scope.blockSize2;
        }

        // Whether frames were lost, the reading thread having fallen a whole buffer behind.
        bool hasOverflowed() const noexcept { return overflowed.load (std::memory_order_relaxed); }

        // The audio thread: a stretch of the device's inputs (by their channels, nullptr for one that is closed, silent).
        void write (const float* const* inputs, int numInputs, int offset, int numSamples) noexcept;

    private:
        const std::vector<int> channels;
        juce::AudioBuffer<float> buffer;
        juce::AbstractFifo fifo;
        std::atomic<bool> overflowed { false };

        Input (const Input&) = delete;
        Input& operator= (const Input&) = delete;
    };

    explicit Recording (std::vector<std::unique_ptr<Input>>);

    const std::vector<std::unique_ptr<Input>>& getInputs() const noexcept { return inputs; }

    // Once it has started (the audio thread has played its first sample): the song's sample that was, and the engine's
    // clock then (Engine::getClock()); and how many samples it has recorded since, without a break. Any thread.
    bool hasStarted() const noexcept { return startSample.load (std::memory_order_acquire) >= 0; }
    std::int64_t getStartSample() const noexcept { return startSample.load (std::memory_order_acquire); }
    std::int64_t getStartClock() const noexcept { return startClock.load (std::memory_order_acquire); }
    std::int64_t getLength() const noexcept { return length.load (std::memory_order_acquire); }
    bool hasEnded() const noexcept { return ended.load (std::memory_order_acquire); }

    // The audio thread: a stretch the song plays, from a sample of the song at a clock, with the device's inputs; and the
    // take's end, as playback stops or jumps, after which it records nothing more.
    void record (const float* const* inputs, int numInputs, int offset, int numSamples, std::int64_t songSample,
                 std::int64_t clock) noexcept;
    void end() noexcept;

private:
    const std::vector<std::unique_ptr<Input>> inputs;
    std::atomic<std::int64_t> startSample { -1 }, startClock { 0 }, length { 0 };
    std::atomic<bool> ended { false };
    static_assert (std::atomic<std::int64_t>::is_always_lock_free);

    Recording (const Recording&) = delete;
    Recording& operator= (const Recording&) = delete;
};
} // namespace phazite::engine
