// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's compressor (roadmap step 1.6): it turns the sound down by the ratio as far as its level goes over the
// threshold, easing in across a soft knee, with an attack, a release and makeup gain. Its detector listens to the sound
// itself, or to a sidechain (another track, for ducking): its key, through a low cut so that bass notes don't pump it, and
// a high cut (off at first) so that a kick ducks a bass and the hats on the same track don't; and it turns both channels
// down alike, so that the stereo image holds. Its meters show how loud the detector hears the key and how far the
// compressor turns the sound down; while its editor asks, it plays the key itself, as its cuts leave it, to hear what it
// listens to.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/MeterValue.h"
#include "devices/core/Processor.h"
#include "devices/dsp/Envelopes.h"
#include "devices/dsp/Filters.h"

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>

namespace phazite::devices
{
class Compressor final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteCompressor";

    enum Parameter
    {
        threshold,
        ratio,
        attack,
        release,
        knee,
        makeup,
        keyLowCut,
        keyHighCut,
        mix // applied by its thin plug-in (DeviceInfo.h, mixParameterID)
    };

    static const DeviceInfo info;

    // How many decibels the compressor turns a steady level (in decibels) down by once it has settled, for the
    // parameters' values in the device's order: its curve, for its editor's graph and for tests.
    static float getReduction (std::span<const float> parameters, float levelDecibels);

    // The loudest the detector heard, in decibels, and the most the compressor turned the sound down by, in decibels
    // (0 or more), since its editor last looked (devices/core/MeterValue.h).
    MeterValue& getLevelMeter() noexcept { return levelMeter; }
    MeterValue& getReductionMeter() noexcept { return reductionMeter; }

    // Message thread: plays the key, as the detector hears it, instead of the sound, for the next half second by the clock;
    // its editor asks again each time it refreshes, so that the key stops when the editor no longer shows, whether the
    // device played meanwhile or not. It fades between the two over a few milliseconds, and its thin plug-in plays it
    // whole, whatever the mix.
    void listenToKey() noexcept
    {
        listenUntil.store (juce::Time::getMillisecondCounterHiRes() + listenMilliseconds, std::memory_order_relaxed);
    }

    bool isPlayingKey() const noexcept { return juce::Time::getMillisecondCounterHiRes() < listenUntil.load (std::memory_order_relaxed); }
    bool overridesMix() const noexcept override { return keyShare > 0.0f; } // the block it last played: the key, or fading
    const FallingValue* getGainReduction() const noexcept override { return &gainReduction; }

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;

private:
    double sampleRate = 44100.0;
    float makeupSmoothing = 0.001f; // of the way to the makeup gain, per sample
    float makeupDecibels = 0.0f;
    float lastGainDecibels = 0.0f, lastGain = 1.0f; // the gain last worked out
    bool isFirstBlock = true;
    static constexpr double listenMilliseconds = 500.0;
    float keyShare = 0.0f, keyStep = 0.01f; // how much of the key it plays, from 0 to 1, and its step per sample

    dsp::LevelFollower reduction;                          // in decibels: rising with the attack, falling with the release
    std::array<dsp::Filter, 2> lowCuts {};                 // the key's, per channel
    std::array<std::array<dsp::Filter, 2>, 2> highCuts {}; // the key's, two sections per channel
    MeterValue levelMeter { -120.0f }, reductionMeter { 0.0f };
    FallingValue gainReduction { reductionFallPerSecond };
    std::atomic<double> listenUntil { 0.0 }; // in the clock's milliseconds

    static_assert (std::atomic<double>::is_always_lock_free);
};
} // namespace phazite::devices
