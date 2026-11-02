// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LevelMeter.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>

namespace phazite::engine
{
void LevelMeter::measure (const float* const* channels, int numChannels, int numSamples) noexcept
{
    numChannels = std::clamp (numChannels, 0, maximumChannels);
    const auto slot = (std::size_t) (blocks.load (std::memory_order_relaxed) % ringSize);

    for (int channel = 0; channel < maximumChannels; ++channel)
    {
        auto peak = 0.0f;

        if (channel < numChannels && channels[channel] != nullptr)
        {
            const auto range = juce::FloatVectorOperations::findMinAndMax (channels[channel], numSamples);
            peak = std::max (-range.getStart(), range.getEnd());
        }

        peaks[(std::size_t) channel][slot].store (peak, std::memory_order_relaxed);
    }

    channelsUsed.store (numChannels, std::memory_order_relaxed);
    blocks.fetch_add (1, std::memory_order_release);
}

float LevelMeter::getPeakSince (Reader& reader, int channel) const noexcept
{
    const auto now = blocks.load (std::memory_order_acquire);

    if (! juce::isPositiveAndBelow (channel, maximumChannels))
        return 0.0f;

    // A reader that waited longer than the ring holds reads what it still holds (the latest blocks but one, which the
    // audio thread may be writing).
    auto& next = reader.next[(std::size_t) channel];
    const auto from = std::max (next, now > (std::uint64_t) ringSize - 1 ? now - (ringSize - 1) : 0);
    auto peak = 0.0f;

    for (auto block = from; block < now; ++block)
        peak = std::max (peak, peaks[(std::size_t) channel][(std::size_t) (block % ringSize)].load (std::memory_order_relaxed));

    next = now;
    return peak;
}
} // namespace phazite::engine
