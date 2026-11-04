// SPDX-License-Identifier: AGPL-3.0-or-later
// A track's modulator or macro in Phazite's engine: a modulator's DSP (devices/modulation/Modulators.h), with its
// parameters' values set by hand, its steps and its switch, or a macro's value, which the message thread sets one at a
// time without building anything; its value on each control point of a block, which what it modulates reads (one every
// 32 samples of the song while it plays, of time while it is stopped); and its values as it played them, which its
// editor shows. Kept by its ID from one plan to the next, so that it runs on through an edit.
#pragma once

#include "devices/modulation/Modulators.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace phazite::engine
{
class ModulatorNode
{
public:
    explicit ModulatorNode (devices::ModulatorKind); // a modulator
    ModulatorNode();                                 // a macro

    bool isMacro() const noexcept { return ! modulator.has_value(); }
    std::optional<devices::ModulatorKind> getKind() const noexcept;

    // Message thread: a parameter's value (by its place in devices::getModulatorParameters()), a step's (-1 to 1), its
    // switch; or a macro's value (0 to 1).
    void setParameter (int index, float value) noexcept;
    void setStep (int index, float value) noexcept;
    void setOn (bool) noexcept;
    bool isOn() const noexcept { return on.load (std::memory_order_relaxed); }
    void setMacroValue (float) noexcept;

    // Before playback, off the audio thread: for blocks of up to so many samples; its random numbers from a seed.
    void prepare (double sampleRate, int maximumBlockSize, std::uint32_t seed);

    // Audio thread. A note started on its track, before a control point; the sound of its track, after its fader, for an
    // envelope follower.
    void noteOn (int pitch, int velocity) noexcept;
    void listen (const float* const* channels, int numChannels, int numSamples) noexcept;

    // Audio thread: its values on the control points of a block, so many from the one at or before the block's start
    // (numbered on their grid: the song's while it plays, time's while it is stopped), each where the song is there, the
    // notes started before each given to it first. A point it gave in the last block is given again as it was.
    struct Point
    {
        std::int64_t index = 0;
        devices::SongPlace place;
        int sample = 0; // from the block's start, where the point is (0 for one before it)
    };

    struct NoteStart
    {
        int sample = 0, pitch = 0, velocity = 0;
    };

    void play (std::span<const Point> points, std::span<const NoteStart> notes, double secondsPerPoint) noexcept;
    std::span<const float> getValues() const noexcept { return { values.data(), (size_t) numValues }; }

    // Any thread: its value as it played it last; and the message thread, its values over the last seconds, oldest first,
    // a hundred a second.
    float getValue() const noexcept { return now.load (std::memory_order_relaxed); }
    std::vector<float> getHistory (double seconds) const;

private:
    std::optional<devices::Modulator> modulator;
    std::unique_ptr<std::atomic<float>[]> parameters;
    std::array<std::atomic<float>, devices::Modulator::maxSteps> steps;
    std::atomic<bool> on { true };
    std::atomic<float> macroValue { 0.0f };
    static_assert (std::atomic<float>::is_always_lock_free);

    // The audio thread's: its parameters and steps for a block, its values on the block's points, the last two points it
    // gave (a block that ends between points gives the one after its end, and the next block starts from the one before),
    // given again as they were, and a macro's value as the last block ended (which it glides on from).
    std::vector<float> blockParameters, blockSteps, values;
    int numValues = 0;
    static constexpr std::int64_t noPoint = std::numeric_limits<std::int64_t>::min() / 2;
    std::int64_t lastIndex = noPoint;
    std::array<float, 2> lastValues {};
    float lastMacro = 0.0f;
    bool macroStarted = false;

    // What it played, a hundred values a second, for its editor: written by the audio thread, read by the message thread.
    static constexpr int historySize = 300, historyRate = 100;
    std::unique_ptr<std::atomic<float>[]> history;
    std::atomic<int> historyEnd { 0 };
    std::atomic<float> now { 0.0f };
    double sampleRate = 44100.0, sinceHistory = 0.0;
};
} // namespace phazite::engine
