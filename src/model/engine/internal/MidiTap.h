// SPDX-License-Identifier: AGPL-3.0-or-later
// What the MIDI inputs send that MIDI learn takes (model/midi/MidiLinkModel.h), before it is played live: a controller
// linked to a parameter (diverted), or, while a parameter learns, any controller that can be linked (0 to 119). Those go
// into a queue for the message thread, which wakes to take them, and nowhere else: not to the instrument, nor into a take
// or Capture. AudioIO (AudioIO.h) holds one; the session the engine plays (model/playback/internal/PlaybackSession.h) says
// what it takes and takes what comes. The inputs' threads, each at once, take no lock and wait for nothing.
#pragma once

#include "engine/core/EventQueue.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace phazite
{
class MidiTap final : private juce::AsyncUpdater
{
public:
    static constexpr int numChannels = 16, numLinkable = 120;

    // A controller's value as it came: its channel (1 to 16), its number (0 to 119) and its value (0 to 127).
    struct Value
    {
        std::uint8_t channel = 1, number = 0, value = 0;
    };

    MidiTap();
    ~MidiTap() override;

    // Any thread (a MIDI input's): whether the message is taken, a controller diverted or, while learning, one that can be
    // linked, which the caller then plays no further. One the queue has no room for is dropped (getDropped()), and is taken
    // all the same.
    bool take (const juce::MidiMessage&) noexcept;

    // Message thread: the controllers taken from now on, by channel and number (others are left out), and whether one is
    // being learned. Neither drops what waits.
    void setDiverted (const std::vector<std::pair<int, int>>& controllers);
    void setLearning (bool) noexcept;
    bool isDiverted (int channel, int number) const noexcept;
    bool isLearning() const noexcept { return learning.load (std::memory_order_relaxed); }

    // Message thread, the one reader: the oldest value waiting, if there is one; and what waits, dropped.
    bool pop (Value&) noexcept;
    void clear() noexcept;

    // How many values the queue had no room for, since the tap was made (any thread).
    std::uint32_t getDropped() const noexcept { return dropped.load (std::memory_order_relaxed); }

    // Called on the message thread soon after a value came (a turn of the message loop), to take what waits; and who set it
    // (the session the engine plays). Set on the message thread.
    std::function<void()> onArrived;
    const void* takenBy = nullptr;

    // Room for a knob swept on several inputs at once (a thousand values a second each) between two turns of the message
    // loop, many times over.
    static constexpr std::size_t capacity = 4096;

private:
    static constexpr int wordsPerChannel = 2; // 120 bits
    std::array<std::atomic<std::uint64_t>, (size_t) (numChannels * wordsPerChannel)> diverted {};
    std::atomic<bool> learning { false };
    std::atomic<std::uint32_t> dropped { 0 };
    engine::EventQueue<Value, capacity> queue;

    void handleAsyncUpdate() override;

    JUCE_DECLARE_NON_COPYABLE (MidiTap)
};

static_assert (std::atomic<std::uint64_t>::is_always_lock_free);
static_assert (std::atomic<std::uint32_t>::is_always_lock_free);
} // namespace phazite
