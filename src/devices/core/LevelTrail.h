// SPDX-License-Identifier: AGPL-3.0-or-later
// How a dynamics device's levels went, finely, for its editor's history (ui/devices/LevelHistoryView.h): a reading for every
// half millisecond it played, each the loudest that came in and went out over it (amplitudes), the most it turned the sound
// down and up (in decibels, 0 or more), and the loudest its detector heard (an amplitude). So the history has a reading for
// every pixel column of its two bars at any tempo and width: one reading for each of the 30 refreshes a second, the loudest
// since the one before, left 3 to 5 pixels between them, which drew as stairs and narrowed each duck's dip to a notch.
//
// The audio thread adds each sample and pushes a reading as each half millisecond ends (counted in samples across blocks),
// without waiting, dropping what doesn't fit; the message thread pulls what it hasn't seen yet. Lock-free, for one writer and
// one reader. It takes readings only while a display listens (setListening()), and holds a second of them, made the first
// time one does, on the message thread, and kept while the device lives.
#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>

namespace phazite::devices
{
struct LevelReading
{
    float input = 0.0f, output = 0.0f; // the loudest samples in and out
    float down = 0.0f, up = 0.0f;      // in decibels, 0 or more
    float heard = 0.0f;                // the loudest its detector heard
};

class LevelTrail
{
public:
    static constexpr double secondsPerReading = 0.0005;
    static constexpr int capacity = 2001; // a second of readings (a fifo holds one less than its size)

    // Message thread: whether a display wants the readings. Makes the store the first time; starting, drops what it held.
    void setListening (bool shouldListen)
    {
        if (shouldListen && owned == nullptr)
        {
            owned = std::make_unique<Store>();
            store.store (owned.get(), std::memory_order_release);
        }

        if (owned == nullptr)
            return;

        if (shouldListen && ! owned->listening.load (std::memory_order_acquire))
            drain();

        owned->listening.store (shouldListen, std::memory_order_release);
    }

    // Either thread.
    bool isListening() const noexcept
    {
        const auto* shown = store.load (std::memory_order_acquire);
        return shown != nullptr && shown->listening.load (std::memory_order_acquire);
    }

    // Audio thread: whether a reading would be kept. Not while its display, hidden, takes none and its second is full, so
    // that the device then notes nothing either.
    bool wantsReadings() const noexcept
    {
        const auto* shown = store.load (std::memory_order_acquire);
        return shown != nullptr && shown->listening.load (std::memory_order_acquire) && shown->fifo.getFreeSpace() > 0;
    }

    // Message thread, the reader: drops what it holds unread, so that the next pull starts from what comes now.
    void drain() noexcept
    {
        if (owned != nullptr)
            owned->fifo.finishedRead (owned->fifo.getNumReady());
    }

    // Message thread: up to maximum readings, oldest first. Returns how many it gave.
    int pull (LevelReading* into, int maximum) noexcept
    {
        if (owned == nullptr)
            return 0;

        const auto scope = owned->fifo.read (std::min (maximum, owned->fifo.getNumReady()));
        scope.forEach ([this, into, done = 0] (int index) mutable { into[done++] = owned->readings[(size_t) index]; });
        return scope.blockSize1 + scope.blockSize2;
    }

    // As the device is prepared: a reading every this many samples.
    void prepare (double sampleRate) noexcept { samplesPerReading = std::max (1, (int) std::lround (sampleRate * secondsPerReading)); }

    // Audio thread, while listening: a sample's levels.
    void add (float input, float output, float down, float up, float heard) noexcept
    {
        building.input = std::max (building.input, input);
        building.output = std::max (building.output, output);
        building.down = std::max (building.down, down);
        building.up = std::max (building.up, up);
        building.heard = std::max (building.heard, heard);

        if (++samples < samplesPerReading)
            return;

        if (auto* shown = store.load (std::memory_order_acquire); shown != nullptr && shown->fifo.getFreeSpace() > 0)
        {
            const auto scope = shown->fifo.write (1);
            scope.forEach ([shown, this] (int index) { shown->readings[(size_t) index] = building; });
        }

        building = {};
        samples = 0;
    }

private:
    struct Store
    {
        juce::AbstractFifo fifo { capacity };
        std::array<LevelReading, (size_t) capacity> readings {};
        std::atomic<bool> listening { false };
    };

    std::unique_ptr<Store> owned; // the message thread's
    std::atomic<Store*> store { nullptr };
    int samplesPerReading = 24, samples = 0; // the audio thread's
    LevelReading building;
};
} // namespace phazite::devices
