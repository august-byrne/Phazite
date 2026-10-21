// SPDX-License-Identifier: AGPL-3.0-or-later
// What goes into a device and what comes out of it, for its editor's displays (an analyzer, a scope, a history of levels):
// the sound itself, each channel apart (StereoTap), and how loud it peaks, in decibels (MeterValue), as the device's thin
// plug-in hands them over each block, its output after its mix. It costs nothing until an editor listens: the taps are made
// then, and kept while the device lives, and the audio thread skips all of it while none listens. One editor listens at a
// time. An instrument has no input.
#pragma once

#include "MeterValue.h"
#include "SampleTap.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

namespace phazite::devices
{
class DeviceScope
{
public:
    static constexpr float silentDecibels = -120.0f;

    // Message thread: whether an editor wants what the device plays. Makes the taps the first time.
    void setListening (bool shouldListen)
    {
        if (shouldListen && ownedInput == nullptr)
        {
            ownedInput = std::make_unique<StereoTap>();
            ownedOutput = std::make_unique<StereoTap>();
            input.store (ownedInput.get(), std::memory_order_release);
            output.store (ownedOutput.get(), std::memory_order_release);
        }

        if (ownedInput != nullptr)
        {
            ownedInput->setListening (shouldListen);
            ownedOutput->setListening (shouldListen);
        }

        listening.store (shouldListen, std::memory_order_release);
    }

    bool isListening() const noexcept { return listening.load (std::memory_order_acquire); }

    // The rate the device plays at, for an analyzer's frequencies (its thin plug-in sets it as playback is prepared).
    void setSampleRate (double rate) noexcept { sampleRate.store (rate, std::memory_order_relaxed); }
    double getSampleRate() const noexcept { return sampleRate.load (std::memory_order_relaxed); }

    // Message thread: the sound, while listening (nullptr before the first time).
    StereoTap* getInput() noexcept { return ownedInput.get(); }
    StereoTap* getOutput() noexcept { return ownedOutput.get(); }

    // The loudest sample since the editor last looked, in decibels (silentDecibels for silence or while nobody listens).
    MeterValue& getInputPeak() noexcept { return inputPeak; }
    MeterValue& getOutputPeak() noexcept { return outputPeak; }

    // Audio thread: a block's sound, going in (an effect's) and coming out.
    void pushInput (const float* left, const float* right, int numSamples) noexcept { push (input, inputPeak, left, right, numSamples); }

    void pushOutput (const float* left, const float* right, int numSamples) noexcept { push (output, outputPeak, left, right, numSamples); }

private:
    std::unique_ptr<StereoTap> ownedInput, ownedOutput; // the message thread's
    std::atomic<StereoTap*> input { nullptr }, output { nullptr };
    std::atomic<bool> listening { false };
    std::atomic<double> sampleRate { 48000.0 };
    MeterValue inputPeak { silentDecibels }, outputPeak { silentDecibels };

    static_assert (std::atomic<double>::is_always_lock_free);

    void push (std::atomic<StereoTap*>& tap, MeterValue& peak, const float* left, const float* right, int numSamples) noexcept
    {
        if (! isListening())
            return;

        auto loudest = 0.0f;

        for (int i = 0; i < numSamples; ++i)
            loudest = std::max ({ loudest, std::abs (left[i]), std::abs (right[i]) });

        peak.raise (loudest > 1.0e-6f ? 20.0f * std::log10 (loudest) : silentDecibels);

        if (auto* shown = tap.load (std::memory_order_acquire))
            shown->push (left, right, numSamples);
    }
};
} // namespace phazite::devices
