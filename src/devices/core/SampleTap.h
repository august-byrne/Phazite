// SPDX-License-Identifier: AGPL-3.0-or-later
// What a device just played, for a display on the message thread, such as an EQ's analyzer: the audio thread pushes
// without waiting and drops what doesn't fit, and the message thread pulls what it hasn't seen yet. Lock-free, for one
// writer and one reader. It takes samples only while a display listens (setListening()), so it costs nothing otherwise.
//
// Each holds a quarter of a second at the highest rate Phazite plays at, 192 kHz (known bug 120): a display reads it at each
// of the 30 refreshes a second, and one that comes a little late loses nothing. (At 8,192 pairs, 43 ms at 192 kHz, a late
// refresh lost samples, which an analyzer showed as a flash.) It is allocated up front, on the message thread.
#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>
#include <vector>

namespace phazite::devices
{
//==============================================================================
// Two channels kept apart, each left sample with its right one, for a display that compares them (a vectorscope) or shows
// each (an analyzer, which shows the louder of the two, so that a sound panned hard or out of phase between them shows at
// its level).
class StereoTap
{
public:
    static constexpr double heldSeconds = 0.25, highestRate = 192000.0;
    static constexpr int defaultCapacity = (int) (heldSeconds * highestRate) + 1; // a fifo holds one less than its size

    explicit StereoTap (int capacity = defaultCapacity)
        : fifo (capacity),
          lefts ((size_t) capacity),
          rights ((size_t) capacity)
    {
    }

    // Message thread, as SampleTap's.
    void setListening (bool shouldListen) noexcept
    {
        if (shouldListen && ! isListening())
            fifo.finishedRead (fifo.getNumReady());

        listening.store (shouldListen, std::memory_order_release);
    }

    bool isListening() const noexcept { return listening.load (std::memory_order_acquire); }

    // Message thread, the reader: drops what it holds unread, so that the next pull starts from what comes now. A display
    // shown again does it: what the tap held, from just after it was hidden, is long gone by.
    void drain() noexcept { fifo.finishedRead (fifo.getNumReady()); }

    // Audio thread (pass the same channel twice for mono).
    void push (const float* left, const float* right, int numSamples) noexcept
    {
        if (! isListening())
            return;

        const auto scope = fifo.write (std::min (numSamples, fifo.getFreeSpace()));
        int done = 0;

        for (const auto& [start, size] :
             { std::pair { scope.startIndex1, scope.blockSize1 }, std::pair { scope.startIndex2, scope.blockSize2 } })
        {
            for (int i = 0; i < size; ++i, ++done)
            {
                lefts[(size_t) (start + i)] = left[done];
                rights[(size_t) (start + i)] = right[done];
            }
        }
    }

    // Message thread: up to maximum pairs, oldest first. Returns how many it gave.
    int pull (float* left, float* right, int maximum) noexcept
    {
        const auto scope = fifo.read (std::min (maximum, fifo.getNumReady()));
        int done = 0;

        for (const auto& [start, size] :
             { std::pair { scope.startIndex1, scope.blockSize1 }, std::pair { scope.startIndex2, scope.blockSize2 } })
        {
            for (int i = 0; i < size; ++i, ++done)
            {
                left[done] = lefts[(size_t) (start + i)];
                right[done] = rights[(size_t) (start + i)];
            }
        }

        return done;
    }

private:
    juce::AbstractFifo fifo;
    std::vector<float> lefts, rights;
    std::atomic<bool> listening { false };
};

//==============================================================================
// A device's own tap, which it holds (an EQ's output, for its analyzer): a StereoTap, made the first time a display listens,
// on the message thread, so that a device whose editor never opens holds nothing; kept while the device lives.
class SampleTap
{
public:
    explicit SampleTap (int pairs = StereoTap::defaultCapacity)
        : capacity (pairs)
    {
    }

    // Message thread: whether a display wants the samples, as StereoTap's.
    void setListening (bool shouldListen)
    {
        if (shouldListen && owned == nullptr)
        {
            owned = std::make_unique<StereoTap> (capacity);
            scratch.resize ((size_t) capacity);
            tap.store (owned.get(), std::memory_order_release);
        }

        if (owned != nullptr)
            owned->setListening (shouldListen);
    }

    // Either thread.
    bool isListening() const noexcept
    {
        const auto* shown = tap.load (std::memory_order_acquire);
        return shown != nullptr && shown->isListening();
    }

    // Message thread: as StereoTap's.
    void drain() noexcept
    {
        if (owned != nullptr)
            owned->drain();
    }

    // Audio thread (pass the same channel twice for mono).
    void push (const float* left, const float* right, int numSamples) noexcept
    {
        if (auto* shown = tap.load (std::memory_order_acquire))
            shown->push (left, right, numSamples);
    }

    // Message thread: up to maximum pairs, oldest first, each channel apart; or the two channels' average. Returns how many
    // it gave.
    int pull (float* left, float* right, int maximum) noexcept { return owned != nullptr ? owned->pull (left, right, maximum) : 0; }

    int pull (float* destination, int maximum) noexcept
    {
        if (owned == nullptr)
            return 0;

        const auto done = owned->pull (destination, scratch.data(), std::min (maximum, (int) scratch.size()));

        for (int i = 0; i < done; ++i)
            destination[i] = 0.5f * (destination[i] + scratch[(size_t) i]);

        return done;
    }

private:
    const int capacity;
    std::unique_ptr<StereoTap> owned; // the message thread's
    std::atomic<StereoTap*> tap { nullptr };
    std::vector<float> scratch; // the right channel, for an average
};
} // namespace phazite::devices
