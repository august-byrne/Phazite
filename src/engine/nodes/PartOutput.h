// SPDX-License-Identifier: AGPL-3.0-or-later
// A part of what a device plays that goes to a mixer channel of its own, as Phazite's engine plays it: a drum sampler's pad
// on a track of its own (model/devices/internal/DrumKit.h), as FL Studio's FPC sends a pad to a mixer track. The device
// plays the part into this buffer rather than into its strip (devices::ProcessContext::partOutputs), and the channel's strip
// adds it up as it adds up another strip (Plan::Input::part). Kept by the device's ID and the part from one plan to the next,
// made on the message thread, and handed to both in the plan.
#pragma once

#include "devices/core/Processor.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace phazite::engine
{
// How many parts a device has at most, numbered from 0 (a drum sampler's 128 pads).
inline constexpr int maximumParts = 128;

class PartOutput
{
public:
    // Message thread: for blocks of up to so many samples. Its channels are taken once, here: after that only the audio
    // thread touches them, through those pointers, never through the buffer's own calls, which write its flags (a plan is
    // made on the message thread while another plays the same output). Not AudioBuffer::clear() either, which skips a
    // buffer it thinks is clear when the device has played into it through the pointers.
    PartOutput (int partNumber, int maximumBlockSize)
        : storage (2, std::max (1, maximumBlockSize)),
          part (partNumber),
          size (storage.getNumSamples())
    {
        storage.clear();
        channels[0] = storage.getWritePointer (0);
        channels[1] = storage.getWritePointer (1);
    }

    int getPart() const noexcept { return part; }
    int getMaximumBlockSize() const noexcept { return size; }

    // What the device is handed of it: its part and its two channels, from the block's start.
    devices::PartOutput describe() const noexcept { return { part, { channels[0], channels[1] } }; }

    // Audio thread: one of its channels, from the block's start, for the strip that adds it up.
    const float* getChannel (int channel) const noexcept { return channels[channel]; }

    // Audio thread: silent where a block is to be played, as the strip of its device starts it.
    void clear (int offset, int numSamples) noexcept
    {
        for (auto* channel : channels)
            juce::FloatVectorOperations::clear (channel + offset, numSamples);
    }

private:
    juce::AudioBuffer<float> storage;
    int part, size;
    float* channels[2] {};
};
} // namespace phazite::engine
