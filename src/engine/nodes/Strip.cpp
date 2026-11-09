// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Strip.h"

#include <algorithm>

namespace phazite::engine
{
void StripState::prepare (double sampleRate, int maximumBlockSize)
{
    buffer.setSize (4, maximumBlockSize);
    buffer.clear();
    beforeFader.setSize (2, maximumBlockSize);
    beforeFader.clear();
    beforeMute.setSize (2, maximumBlockSize);
    beforeMute.clear();
    scratch.setSize (2, 9 * maximumBlockSize + 16);
    noteStarts.reserve ((size_t) DeviceRunner::maximumEvents);
    volumeModulation.assign ((size_t) (maximumBlockSize / controlPointSpacing + 3), 0.0f);
    panModulation.assign (volumeModulation.size(), 0.0f);
    channel.prepare (sampleRate);
    sequence = nullptr;
    nextEvent = 0;
    sounding.reset();
    mustChase = true;
    wasSilenced = false;
    numEvents = 0;
}

void StripState::addEvent (devices::NoteEvent event) noexcept
{
    if (numEvents < (int) events.size())
        events[(std::size_t) numEvents++] = event;
}

void StripState::sortEvents() noexcept
{
    // By sample, keeping the order they were added in at a sample: an insertion sort, as they come mostly in order.
    for (int i = 1; i < numEvents; ++i)
    {
        const auto event = events[(std::size_t) i];
        auto j = i;

        for (; j > 0 && events[(std::size_t) j - 1].sample > event.sample; --j)
            events[(std::size_t) j] = events[(std::size_t) j - 1];

        events[(std::size_t) j] = event;
    }
}

//==============================================================================
Delay::Delay (int samples)
    : length (std::max (0, samples)),
      line (2, std::max (1, samples))
{
    line.clear();
}

void Delay::addDelayed (const float* const* inputs, float* const* outputs, int numSamples, float startGain, float endGain) noexcept
{
    // At a gain: a straight line from one to the other over the block.
    const auto add = [numSamples, startGain, endGain] (float* to, const float* from, int offset, int count)
    {
        if (juce::exactlyEqual (startGain, 1.0f) && juce::exactlyEqual (endGain, 1.0f))
        {
            juce::FloatVectorOperations::add (to, from, count);
            return;
        }

        const auto step = (endGain - startGain) / (float) numSamples;

        for (int i = 0; i < count; ++i)
            to[i] += from[i] * (startGain + step * (float) (offset + i));
    };

    if (length == 0)
    {
        for (int channel = 0; channel < 2; ++channel)
            add (outputs[channel], inputs[channel], 0, numSamples);

        return;
    }

    for (int done = 0; done < numSamples;)
    {
        // As far as the end of the line at most: what it kept there goes out, and what comes in takes its place.
        const auto part = std::min (numSamples - done, length - position);

        for (int channel = 0; channel < 2; ++channel)
        {
            auto* kept = line.getWritePointer (channel, position);
            add (outputs[channel] + done, kept, done, part);
            juce::FloatVectorOperations::copy (kept, inputs[channel] + done, part);
        }

        position = (position + part) % length;
        done += part;
    }
}

void Delay::reset() noexcept
{
    line.clear();
    position = 0;
}

//==============================================================================
void SendLevel::setDecibels (float decibels) noexcept
{
    gain.store (decibels <= Channel::silenceDb ? 0.0f : juce::Decibels::decibelsToGain (decibels, Channel::silenceDb),
                std::memory_order_relaxed);
}

std::pair<float, float> SendLevel::moveOn() noexcept
{
    const auto now = gain.load (std::memory_order_relaxed);
    const auto from = applied < 0.0f ? now : applied;
    applied = now;
    return { from, now };
}

} // namespace phazite::engine
