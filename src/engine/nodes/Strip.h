// SPDX-License-Identifier: AGPL-3.0-or-later
// A track, a bus, a return or the master as Phazite's engine plays it (docs/tracktion-migration.md, T2), kept by its ID
// from one plan to the next: its channel (Channel.h), the buffer it plays into, what it sends before its fader and what a
// sidechain hears of it, and where its notes are as it plays them. And what joins paths: the delay that lines a path up
// with a later one (delay compensation), a send's level, and a container's mix of its devices' sound with what came in.
#pragma once

#include "Channel.h"
#include "DeviceRunner.h"
#include "NoteSequence.h"
#include "engine/modulation/ModulatorNode.h"

#include <array>
#include <atomic>
#include <bitset>
#include <memory>
#include <utility>
#include <vector>

namespace phazite::engine
{
class StripState
{
public:
    // Message thread, before a plan points at it: buffers for blocks of up to so many samples.
    void prepare (double sampleRate, int maximumBlockSize);

    Channel& getChannel() noexcept { return channel; }

    // The audio thread's: what it plays, on two channels, and on two more what a device's sidechain hears; its sound before
    // its fader, for sends before the fader; its sound after its fader and before its mute, for sidechains; and what its
    // audio clips read (AudioRegion::play()).
    juce::AudioBuffer<float> buffer, beforeFader, beforeMute, scratch;

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

    // What its modulators hear of its notes in a block, the notes started; and its fader's modulation on each of the block's
    // control points (Channel::Automation).
    std::vector<ModulatorNode::NoteStart> noteStarts;
    std::vector<float> volumeModulation, panModulation;

    // The engine's pass it has played last (Engine.h), which the strips that take it in wait for; and for one whose
    // instrument is handed its blocks early, the pass whose block it was last handed, which the strip's own task waits for,
    // and whether it plays nothing in it.
    std::atomic<std::uint64_t> donePass { 0 }, startedPass { 0 };
    bool startedSilent = false;

private:
    Channel channel;
};

// A delay of a fixed length on two channels.
class Delay
{
public:
    explicit Delay (int samples);

    int getLength() const noexcept { return length; }

    // Audio thread: adds what came in so many samples ago to the outputs, at a gain going in a straight line from one to
    // another over the block, and keeps what comes in now.
    void addDelayed (const float* const* inputs, float* const* outputs, int numSamples, float startGain = 1.0f,
                     float endGain = 1.0f) noexcept;

private:
    int length = 0, position = 0;
    juce::AudioBuffer<float> line;
};

// A send's level, which the message thread sets and the audio thread glides to over a block. Kept by the send's ID.
class SendLevel
{
public:
    void setDecibels (float decibels) noexcept;

    // Audio thread: the gain as the last block ended, and as this one does.
    std::pair<float, float> moveOn() noexcept;

private:
    std::atomic<float> gain { 1.0f };
    float applied = -1.0f; // none yet
};

// A container of effects (model/mixer/ContainerModel.h): what comes into its devices, kept and delayed as much as they
// delay their sound, to mix with what they put out by its mix, which the message thread sets and the audio thread glides
// to over a block. Kept by the container's ID.
class ContainerState
{
public:
    ContainerState (int maximumBlockSize, int latency);

    int getLatency() const noexcept { return latency; }
    void setMix (float) noexcept;

    // Audio thread: what comes in, before its first device; and after its last, its devices' sound mixed with it.
    void keepDry (const float* const* channels, int numSamples) noexcept;
    void mix (float* const* channels, int numSamples) noexcept;

private:
    int latency = 0;
    std::atomic<float> mixValue { 1.0f };
    float applied = -1.0f;
    juce::AudioBuffer<float> dry;
    std::unique_ptr<Delay> delay;
};
} // namespace phazite::engine
