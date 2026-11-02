// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Recording.h"

namespace phazite::engine
{
Recording::Input::Input (std::vector<int> inputChannels, int capacity)
    : channels (std::move (inputChannels)),
      buffer (std::max (1, (int) channels.size()), std::max (1, capacity)),
      fifo (std::max (1, capacity))
{
}

void Recording::Input::write (const float* const* inputs, int numInputs, int offset, int numSamples) noexcept
{
    if (fifo.getFreeSpace() < numSamples)
    {
        overflowed.store (true, std::memory_order_relaxed);
        return;
    }

    const auto scope = fifo.write (numSamples);

    for (size_t c = 0; c < channels.size(); ++c)
    {
        const auto channel = channels[c];
        const auto* in = channel >= 0 && channel < numInputs && inputs[channel] != nullptr ? inputs[channel] + offset : nullptr;

        const auto part = [this, c, in] (int start, int count, int from)
        {
            if (count <= 0)
                return;

            if (in != nullptr)
                buffer.copyFrom ((int) c, start, in + from, count);
            else
                buffer.clear ((int) c, start, count);
        };

        part (scope.startIndex1, scope.blockSize1, 0);
        part (scope.startIndex2, scope.blockSize2, scope.blockSize1);
    }
}

//==============================================================================
Recording::Recording (std::vector<std::unique_ptr<Input>> recorded)
    : inputs (std::move (recorded))
{
}

void Recording::record (const float* const* deviceInputs, int numInputs, int offset, int numSamples, std::int64_t songSample,
                        std::int64_t clock) noexcept
{
    if (ended.load (std::memory_order_relaxed) || numSamples <= 0)
        return;

    for (const auto& input : inputs)
        input->write (deviceInputs, numInputs, offset, numSamples);

    // Its start, once: what a reader takes after it sees the start is the start's.
    if (startSample.load (std::memory_order_relaxed) < 0)
    {
        startClock.store (clock, std::memory_order_relaxed);
        startSample.store (songSample, std::memory_order_release);
    }

    length.store (length.load (std::memory_order_relaxed) + numSamples, std::memory_order_release);
}

void Recording::end() noexcept
{
    if (startSample.load (std::memory_order_relaxed) >= 0)
        ended.store (true, std::memory_order_release);
}
} // namespace phazite::engine
