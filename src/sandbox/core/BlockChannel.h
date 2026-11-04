// SPDX-License-Identifier: AGPL-3.0-or-later
// The channel a plug-in instance's blocks go through, in memory Phazite and the helper share: the block Phazite's
// engine hands over (its audio, processed in place, its sidechain, its MIDI, its parameters' changes at their samples,
// and where the song is), and the helper's answer in the same place. A block is one round trip: the engine rings the
// helper's bell with the block's number and waits on its own bell until the helper rings it with the same number, at
// most until a deadline; a block the helper answers late stays the helper's until it does. Made by Phazite, opened by
// the helper. Real-time safe on both sides once made.
#pragma once

#include "SharedMemory.h"

#include <cstdint>
#include <memory>
#include <string>

namespace phazite::sandbox
{
// A MIDI message at its sample in the block.
struct MidiEvent
{
    std::int32_t sample = 0;
    std::uint8_t size = 0;
    std::uint8_t bytes[3] {};
};

// A parameter's new value from a sample in the block on: by its index among the instance's parameters, from 0 to 1.
struct ParameterChange
{
    std::int32_t index = 0;
    std::int32_t sample = 0;
    float value = 0.0f;
};

// What comes with a block, besides its data.
struct BlockInfo
{
    std::int32_t numSamples = 0;
    std::int32_t numChannels = 0; // in, and out in place
    std::int32_t numSidechainChannels = 0;
    std::int32_t numEvents = 0;
    std::int32_t numChanges = 0;
    std::int32_t timeSignatureNumerator = 4, timeSignatureDenominator = 4;
    std::int32_t isPlaying = 0, isRendering = 0;
    std::int32_t reset = 0; // the plug-in reset before it plays the block: what rings on from before stops
    double tempo = 120.0;
    double beat = 0.0;       // where the song is at the block's start, in quarter notes
    std::int64_t sample = 0; // and in samples
};

class BlockChannel
{
public:
    struct Limits
    {
        std::int32_t maxSamples = 0, maxChannels = 2, maxSidechainChannels = 2, maxEvents = 1024, maxChanges = 1024;
    };

    // Made under a name of at most 20 characters, or opened by it with the limits it was made with; nullptr if it couldn't be.
    static std::unique_ptr<BlockChannel> create (const std::string& name, const Limits&);
    static std::unique_ptr<BlockChannel> open (const std::string& name, const Limits&);

    const Limits& getLimits() const noexcept { return limits; }

    // The block, in shared memory: written by the engine before play(), by the helper before finish().
    BlockInfo& getInfo() noexcept { return header->info; }
    float* getChannel (int channel) noexcept { return audio + (std::size_t) channel * (std::size_t) limits.maxSamples; }
    float* getSidechain (int channel) noexcept { return getChannel (limits.maxChannels + channel); }
    MidiEvent* getEvents() noexcept { return events; }
    ParameterChange* getChanges() noexcept { return changes; }

    //==============================================================================
    // The engine's side.
    enum class Played
    {
        done, // answered: its audio is the helper's output
        late, // not answered by the deadline: the helper keeps it until it is
        busy  // the block before is still unanswered: nothing handed over
    };

    // Hands the block over and waits for its answer: spinning for spinNanoseconds, then asleep until timeoutNanoseconds
    // (negative: for as long as it takes, for a render).
    Played play (std::int64_t spinNanoseconds, std::int64_t timeoutNanoseconds) noexcept;

    // The two halves of play(), for the blocks of several instances handed over first and their answers waited for after,
    // so that they play side by side: false (busy) if the block before is still unanswered.
    bool handOver() noexcept;
    Played waitForAnswer (std::int64_t spinNanoseconds, std::int64_t timeoutNanoseconds) noexcept;

    // Whether the block handed over last is still the helper's.
    bool isBusy() const noexcept { return replyBell->get() != handed; }

    //==============================================================================
    // The helper's side: waits for the next block, at most for timeoutNanoseconds (negative: no limit); then answers it.
    bool waitForBlock (std::int64_t timeoutNanoseconds) noexcept;
    void finish() noexcept;

    BlockChannel (const BlockChannel&) = delete;
    BlockChannel& operator= (const BlockChannel&) = delete;

private:
    struct PaddedBell
    {
        BellState state;
        char padding[64 - sizeof (BellState)] {}; // the rest of a cache line
    };

    struct Header
    {
        PaddedBell request, reply; // each on a cache line of its own: one side writes each
        Limits limits;
        BlockInfo info;
    };

    BlockChannel() = default;
    static std::unique_ptr<BlockChannel> make (const std::string& name, const Limits&, bool create);

    std::unique_ptr<SharedMemory> memory;
    std::unique_ptr<Bell> requestBell, replyBell;
    Limits limits;
    Header* header = nullptr;
    float* audio = nullptr;
    MidiEvent* events = nullptr;
    ParameterChange* changes = nullptr;
    std::uint32_t handed = 0; // the engine's: the last block it handed over
    std::uint32_t taken = 0;  // the helper's: the last block it took
};
} // namespace phazite::sandbox
