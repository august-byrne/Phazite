// SPDX-License-Identifier: AGPL-3.0-or-later
// A container as Phazite's engine plays it (model/mixer/ContainerModel.h): its chains, each a run of a strip's devices with
// a buffer of its own, which play side by side, each on its part of the sound that comes in (Split), through its own fader
// (Channel.h: its volume, its pan and whether it is heard), lined up with the latest and added up; and what came in, kept
// and delayed as much, to mix with them by the container's mix. An instrument container's chains play the strip's notes
// from silence, each those in its key and velocity ranges. Each is kept by its ID from one plan to the next.
#pragma once

#include "Strip.h"
#include "devices/core/DeviceInfo.h"
#include "devices/dsp/Filters.h"
#include "engine/automation/Curve.h"

#include <array>
#include <atomic>
#include <bitset>
#include <memory>
#include <span>
#include <vector>

namespace phazite::engine
{
// How many chains a container plays at most.
inline constexpr int maximumChains = 16;

// What part of the sound coming in each of a container's chains plays: all of it (each chain a layer, which add up); a band
// of it, low to high in their order, each from its chain's crossover (the first from the bottom), Linkwitz-Riley crossovers
// of the fourth order with the lower bands kept in phase with the higher, so that the bands add up to what came in at every
// frequency, through an all-pass (devices/dsp/Crossover.h); its middle, what its sides share, for the first chain, and its
// side, what they don't, for the second; or its left for the first and its right for the second. The parts add up to the
// sound, so chains that pass their parts as they come pass it whole; a chain past the parts there are plays nothing.
enum class Split
{
    layers,
    bands,
    midSide,
    leftRight
};

// A chain's crossover, where its band starts, in hertz, as automation draws it and modulation moves it: from 20 Hz to
// 20 kHz, halfway along its travel at 630 Hz.
inline constexpr devices::ParameterInfo crossoverInfo { .id = "crossover",
                                                        .name = "Crossover",
                                                        .minimum = 20.0f,
                                                        .maximum = 20000.0f,
                                                        .defaultValue = 1000.0f,
                                                        .unit = devices::Unit::hertz,
                                                        .midpoint = 630.0f };

// What comes into a container of effects, kept and delayed as much as its chains delay their sound (its latest chain), to
// mix with what they put out by its mix, which the message thread sets and the audio thread glides to over a block. Kept
// by the container's ID.
class ContainerState
{
public:
    ContainerState (int maximumBlockSize, int latency);

    int getLatency() const noexcept { return latency; }
    void setMix (float) noexcept;

    // Audio thread: what comes in, before its chains; and after them, what they put out mixed with it.
    void keepDry (const float* const* channels, int numSamples) noexcept;
    void mix (float* const* channels, int numSamples) noexcept;
    void reset() noexcept; // nothing kept that came in before

private:
    int latency = 0;
    std::atomic<float> mixValue { 1.0f };
    float applied = -1.0f;
    juce::AudioBuffer<float> dry;
    std::unique_ptr<Delay> delay;
};

// One of a container's chains, kept by the chain's ID: the buffer its devices play in, its fader, its crossover and the
// notes it plays (an instrument container's), which the message thread sets one at a time, without building anything.
class ChainState
{
public:
    // Message thread, before a plan points at it: buffers for blocks of up to so many samples, at a rate.
    void prepare (double sampleRate, int maximumBlockSize);

    // Its volume, pan and whether it is heard (its mute, and its container's other chains' solos, which the model works out).
    Channel& getFader() noexcept { return fader; }
    const Channel& getFader() const noexcept { return fader; }

    // Message thread: its crossover, set by hand (in hertz); whether it is written by hand now, so that it plays that
    // rather than its automation; and the notes it plays, an instrument container's chain's: its keys and its velocities.
    void setCrossover (float hertz) noexcept { crossover.store (hertz, std::memory_order_relaxed); }
    float getCrossover() const noexcept { return crossover.load (std::memory_order_relaxed); }
    void setCrossoverWriting (bool isWriting) noexcept { crossoverWriting.store (isWriting, std::memory_order_relaxed); }
    void setNotes (int lowKey, int highKey, int lowVelocity, int highVelocity) noexcept;

    // Any thread: the crossover it played last, with its modulation or without.
    float getPlayedCrossover (bool withModulation = true) const noexcept
    {
        return (withModulation ? playedCrossover : playedBaseCrossover).load (std::memory_order_relaxed);
    }

    // Audio thread: its crossover at a beat: its automation's there (but while it is written by hand), or else its value set
    // by hand, moved by its modulation, a part of its range; kept as the one it played.
    float findCrossover (const Curve* automation, double beat, std::span<const float> modulation, size_t point) noexcept;

    // Audio thread: whether one of the strip's note events is for this chain: a note in its ranges, which it keeps as it
    // starts; the end of one it started; and anything else (a bend, a controller, every note ending).
    bool takes (const devices::NoteEvent&) noexcept;

    // Audio thread: the notes it started forgotten, and its band's filters silent, as playback starts or jumps.
    void reset() noexcept;

    // The audio thread's: what it plays, on two channels (two more for a sidechain, which a device in a container has not);
    // the events of its instrument in a stretch of a block, in time order; its fader's modulation on each of the block's
    // control points, and its crossover's; and whether it was silent, so that its devices start from silence when it is
    // heard again.
    juce::AudioBuffer<float> buffer;
    std::array<devices::NoteEvent, DeviceRunner::maximumEvents> events {};
    int numEvents = 0;
    std::vector<float> volumeModulation, panModulation, crossoverModulation;
    bool wasSilenced = false;

    // Its band's split, the audio thread's: the sections of its crossover, made for the frequency they were made for last;
    // the filters that split each channel there into what is below and above it (two sections each way); and those that keep
    // its band in phase with the bands above the next one, the all-pass of each chain's crossover there.
    devices::dsp::FilterCoefficients low, high, allPass;
    float madeFor = -1.0f;
    std::array<std::array<devices::dsp::Filter, 4>, 2> split {};
    std::array<std::array<devices::dsp::Filter, maximumChains>, 2> phase {};
    double sampleRate = 48000.0;

private:
    Channel fader;
    juce::NormalisableRange<float> crossoverRange;
    std::atomic<float> crossover { crossoverInfo.defaultValue }, playedCrossover { crossoverInfo.defaultValue },
        playedBaseCrossover { crossoverInfo.defaultValue };
    std::atomic<bool> crossoverWriting { false };
    std::atomic<int> lowKey { 0 }, highKey { 127 }, lowVelocity { 1 }, highVelocity { 127 };
    std::bitset<128> sounding; // the notes it started, which end in it
};

// One of a container's chains, as a split hands it its part: its state, its crossover's automation (none for none) and
// modulation on the block's control points.
struct ChainPart
{
    ChainState* state = nullptr;
    const Curve* crossover = nullptr;
    std::span<const float> crossoverModulation {};
};

// Where the song is as a stretch of a block starts, for the crossovers' automation and modulation, which move on the song's
// control points (Curve.h): its beat, how far it moves in a sample (0 while stopped), and its sample.
struct SplitPosition
{
    double beat = 0.0, beatsPerSample = 0.0;
    std::int64_t sample = 0;
};

// Audio thread: what comes in (two channels, from where the stretch starts) as each chain's part of it, into the chains'
// buffers from that place in them.
void splitIntoParts (Split, const float* const* input, int offset, int numSamples, std::span<const ChainPart>,
                     const SplitPosition&) noexcept;
} // namespace phazite::engine
