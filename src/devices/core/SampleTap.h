// SPDX-License-Identifier: AGPL-3.0-or-later
// What a device just played, for a display on the message thread, such as an EQ's analyzer: the audio thread pushes
// without waiting and drops what doesn't fit, and the message thread pulls what it hasn't seen yet. Lock-free, for one
// writer and one reader. It takes samples only while a display listens (setListening()), so it costs nothing otherwise.
#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <utility>
#include <vector>

namespace phazite::devices
{
class SampleTap
{
public:
    explicit SampleTap (int capacity = 16384)
        : fifo (capacity),
          buffer ((size_t) capacity)
    {
    }

    // Message thread: whether a display wants the samples. A display that starts listening gets only what comes from
    // then on: what an earlier one left is dropped, by the reader's side, which the audio thread doesn't touch.
    void setListening (bool shouldListen) noexcept
    {
        if (shouldListen && ! isListening())
            fifo.finishedRead (fifo.getNumReady());

        listening.store (shouldListen, std::memory_order_release);
    }

    bool isListening() const noexcept { return listening.load (std::memory_order_acquire); }

    // Audio thread: the average of two channels (pass the same one twice for mono).
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
                buffer[(size_t) (start + i)] = 0.5f * (left[done] + right[done]);
        }
    }

    // Message thread: up to maximum samples, oldest first. Returns how many it gave.
    int pull (float* destination, int maximum) noexcept
    {
        const auto scope = fifo.read (std::min (maximum, fifo.getNumReady()));
        int done = 0;

        for (const auto& [start, size] :
             { std::pair { scope.startIndex1, scope.blockSize1 }, std::pair { scope.startIndex2, scope.blockSize2 } })
        {
            for (int i = 0; i < size; ++i, ++done)
                destination[done] = buffer[(size_t) (start + i)];
        }

        return done;
    }

private:
    juce::AbstractFifo fifo;
    std::vector<float> buffer;
    std::atomic<bool> listening { false };
};
} // namespace phazite::devices
