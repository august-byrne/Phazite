// SPDX-License-Identifier: AGPL-3.0-or-later
// What Phazite's engine records of a take: each armed track's audio input, from the first sample the song plays after
// the take is given to the engine (after the count-in) for as long as it plays on without a stop or a jump, into a
// buffer of its own that one reading thread empties; and where in the song that first sample was, with the engine's
// clock then, so that the take's place is known to the sample. While the reading thread has fallen a whole buffer behind
// (a drive that stalls), what comes in is lost, and the buffer notes where and how much, so that the reader puts silence
// there and what comes after keeps its place. The notes played live meanwhile come out of the engine with the clock they
// came in at (Engine::takePlayed()). Made and freed on the message thread, handed to the engine whole
// (Engine::setRecording()); the audio thread writes into it, real-time safe.
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
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

        // The reading thread (one at a time): what has come in, in the order it was played, up to so many frames. What came
        // in is handed to one function, which takes the channels and a count of frames, in one part or more; and frames
        // lost where they were lost, to another, which takes a count of frames of silence, so that what comes after them
        // keeps its place. How many frames it handed, of both.
        template <typename Write, typename Silence>
        std::int64_t read (std::int64_t maxFrames, Write&& write, Silence&& silence);

        // The audio thread: a stretch of the device's inputs (by their channels, nullptr for one that is closed, silent).
        // Without room for all of it in the buffer, it is lost, and noted as lost with what was lost just before it; once
        // something has room again, the gap is handed on before it, so that the reader knows of the gap before it reads
        // what came after. While there is no room to note another gap, what comes in is lost into the one open, which keeps
        // everything after it in its place.
        void write (const float* const* inputs, int numInputs, int offset, int numSamples) noexcept;

    private:
        // A stretch of frames lost: where it started, in frames since the take's start (those that came in and those
        // lost), and how many.
        struct Gap
        {
            std::int64_t start = 0, length = 0;
        };

        static constexpr int maximumGaps = 32; // waiting for the reader at once: one or two, but for a reader that falls behind

        const std::vector<int> channels;
        juce::AudioBuffer<float> buffer;
        juce::AbstractFifo fifo;
        std::array<Gap, maximumGaps> gaps; // noted, for the reader, through gapFifo
        juce::AbstractFifo gapFifo { maximumGaps };
        std::int64_t framesWritten = 0; // the audio thread's: since the take's start, those that came in and those lost
        Gap lost;                       // the audio thread's: what it has lost since it last had room, while not empty
        std::int64_t framesRead = 0;    // the reading thread's: of both

        std::optional<Gap> getNextGap() const noexcept; // the reading thread's: the first noted that it hasn't passed

        Input (const Input&) = delete;
        Input& operator= (const Input&) = delete;
    };

    explicit Recording (std::vector<std::unique_ptr<Input>>);

    const std::vector<std::unique_ptr<Input>>& getInputs() const noexcept { return inputs; }

    // Once it has started (the audio thread has played its first sample): the song's sample that was, and the engine's
    // clock then (Engine::getClock()); and how many samples it has recorded since, without a break (each input's frames,
    // those that came in and those lost). Any thread.
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

//==============================================================================
template <typename Write, typename Silence>
std::int64_t Recording::Input::read (std::int64_t maxFrames, Write&& write, Silence&& silence)
{
    std::int64_t handed = 0;

    while (handed < maxFrames)
    {
        // What has come in, then the first gap noted: a gap is noted before what came after it, so one before anything
        // counted here is known.
        const auto ready = fifo.getNumReady();
        const auto gap = getNextGap();

        // Silence where frames were lost, the gap taken once all of it is.
        if (gap.has_value() && gap->start <= framesRead)
        {
            const auto gapEnd = gap->start + gap->length;
            const auto count = std::min (gapEnd - framesRead, maxFrames - handed);
            silence (count);
            framesRead += count;
            handed += count;

            if (framesRead >= gapEnd)
                gapFifo.finishedRead (1);

            continue;
        }

        // What came in, as far as the next gap.
        auto count = std::min<std::int64_t> (maxFrames - handed, ready);

        if (gap.has_value())
            count = std::min (count, gap->start - framesRead);

        if (count <= 0)
            break;

        const auto scope = fifo.read ((int) count);
        const auto part = [this, &write] (int start, int size)
        {
            if (size <= 0)
                return;

            const float* data[] = { buffer.getReadPointer (0, start), buffer.getReadPointer (buffer.getNumChannels() - 1, start) };
            write (data, (int) channels.size(), size);
        };

        part (scope.startIndex1, scope.blockSize1);
        part (scope.startIndex2, scope.blockSize2);
        framesRead += count;
        handed += count;
    }

    return handed;
}
} // namespace phazite::engine
