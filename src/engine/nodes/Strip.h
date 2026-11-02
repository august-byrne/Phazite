// SPDX-License-Identifier: AGPL-3.0-or-later
// A track or the master as Phazite's engine plays it (docs/tracktion-migration.md, T2), kept by its ID from one plan to
// the next: its channel (Channel.h), the buffer it plays into, and where its notes are as it plays them. And the delay that
// lines a path up with a later one where they join (delay compensation).
#pragma once

#include "Channel.h"
#include "DeviceRunner.h"
#include "NoteSequence.h"

#include <array>
#include <bitset>

namespace phazite::engine
{
class StripState
{
public:
    // Message thread, before a plan points at it: buffers for blocks of up to so many samples.
    void prepare (double sampleRate, int maximumBlockSize);

    Channel& getChannel() noexcept { return channel; }

    // The audio thread's.
    juce::AudioBuffer<float> buffer; // what it plays, on two channels

    // Where its notes are: the sequence played last, its next event, the pitches sounding, and whether the notes sounding
    // where it plays from must start (after a jump, the loop's start, or playback starting).
    const NoteSequence* sequence = nullptr;
    std::size_t nextEvent = 0;
    std::bitset<128> sounding;
    bool mustChase = true;
    bool wasSilenced = false;

    // What its instrument plays in a stretch of a block, in time order.
    std::array<devices::NoteEvent, DeviceRunner::maximumEvents> events {};
    int numEvents = 0;
    void addEvent (devices::NoteEvent) noexcept; // dropped once full
    void sortEvents() noexcept;

private:
    Channel channel;
};

// A delay of a fixed length on two channels.
class Delay
{
public:
    explicit Delay (int samples);

    int getLength() const noexcept { return length; }

    // Audio thread: adds what came in so many samples ago to the outputs, and keeps what comes in now.
    void addDelayed (const float* const* inputs, float* const* outputs, int numSamples) noexcept;

private:
    int length = 0, position = 0;
    juce::AudioBuffer<float> line;
};
} // namespace phazite::engine
