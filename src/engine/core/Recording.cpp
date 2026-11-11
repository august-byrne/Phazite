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
    // Lost without room for it, or while the gap before it can't be noted yet: what comes after a gap waits for it.
    if (fifo.getFreeSpace() < numSamples || (lost.length > 0 && gapFifo.getFreeSpace() < 1))
    {
        if (lost.length == 0)
            lost.start = framesWritten;

        lost.length += numSamples;
        framesWritten += numSamples;
        return;
    }

    // The gap before it, handed on first.
    if (lost.length > 0)
    {
        {
            const auto scope = gapFifo.write (1);
            gaps[(size_t) scope.startIndex1] = lost;
        }

        lost = {};
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

    framesWritten += numSamples;
}

std::optional<Recording::Input::Gap> Recording::Input::getNextGap() const noexcept
{
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    gapFifo.prepareToRead (1, start1, size1, start2, size2);
    return size1 > 0 ? std::optional (gaps[(size_t) start1]) : std::nullopt;
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
