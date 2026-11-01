// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's compressor (roadmap step 1.6): it turns each level as its curve says, drawn as Maximus draws its curves
// (DynamicsCurve.h; the owner, 2026-11-01: "i want this graph to also be used the same in the compressor plugin"), so that it
// compresses or expands, downward or upward, limits or gates, by the curve's shape alone, with an attack (as the gain goes
// down), a release (as it comes back up) and input and makeup gains around it. Its character sets how it hears and reacts, as
// Logic's circuit types and FabFilter's styles do (the owner, 2026-11-01: "depth with different compression types"): Clean,
// which hears each peak and adds no colour of its own; VCA, which hears the average level, so short peaks pass; FET, which
// hears each peak and adds odd harmonics the harder it turns down, as an 1176 does; Opto, which hears the average and lets go
// fast at first and then slowly, the longer it held the slower; and Tube, which hears the average and adds even harmonics the
// harder it turns down. Each hears what comes in (feed-forward), so that once settled it follows its curve exactly whatever
// its character; a compressor that listened to what it put out would settle elsewhere than the curve drawn, and run away
// where the curve expands. Drive saturates what it puts out in the character's colour (FET's odd harmonics, Opto's and Tube's
// even ones, Clean's and VCA's soft clipping), without aliasing much (antiderivative anti-aliasing). Its detector listens to
// the sound itself, or to a sidechain (another track, for ducking): its key, through a low cut so that bass notes don't pump
// it, and a high cut (off at first) so that a kick ducks a bass and the hats on the same track don't; and it turns both
// channels alike, so that the stereo image holds. Its meters show how loud the detector hears the key and how far the
// compressor turns the sound down and up; while its editor asks, it plays the key itself, as its cuts leave it, to hear what
// it listens to.
#pragma once

#include "DynamicsCurve.h"
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
        character, // a Character
        inputGain,
        attack,
        release,
        makeup,
        drive, // from 0 (none) to 1
        keyLowCut,
        keyHighCut,
        points,                               // its curve (DynamicsCurve.h): how many points it has, then its points
        mix = points + curves::parameterCount // applied by its thin plug-in (DeviceInfo.h, mixParameterID)
    };

    enum class Character
    {
        clean, // hears each peak and reacts as set, with no colour of its own
        vca,   // hears the average (RMS) level, so short peaks pass: punchy, for buses
        fet,   // hears each peak; odd harmonics, the more the further it turns down: aggressive
        opto,  // hears the average, with a release that slows as it goes, the more the longer it held
        tube   // hears the average; even harmonics, the more the further it turns down: warm
    };

    static const DeviceInfo info;

    // Its curve, among the parameters' values in the device's order.
    static std::span<const float> getCurve (std::span<const float> parameters)
    {
        return parameters.subspan ((size_t) points, curves::parameterCount);
    }

    // The loudest the detector heard, in decibels, and the most the compressor turned the sound down by and up by, in
    // decibels (0 or more), since its editor last looked (devices/core/MeterValue.h).
    MeterValue& getLevelMeter() noexcept { return levelMeter; }
    MeterValue& getReductionMeter() noexcept { return reductionMeter; }
    MeterValue& getBoostMeter() noexcept { return boostMeter; }

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
    float makeupDecibels = 0.0f, inputDecibels = 0.0f, driveAmount = 0.0f;
    float lastInputDecibels = 0.0f, lastInput = 1.0f;   // the input gain last worked out
    float lastGainDecibels = 0.0f, lastGain = 1.0f;     // the gain last worked out
    float lastLevel = -1.0f, lastWanted = 0.0f;         // the level last heard, and the gain its curve wanted
    float lastMost = 0.0f;                              // the most it turned down in its last block, for its colour
    float lastDriveAmount = -1.0f, driveGain = 1.0f;    // into drive's curve
    float lastMakeupDecibels = 0.0f, makeupGain = 1.0f; // after it
    bool wasDriven = false;
    bool isFirstBlock = true;
    static constexpr double listenMilliseconds = 500.0;
    float keyShare = 0.0f, keyStep = 0.01f; // how much of the key it plays, from 0 to 1, and its step per sample

    curves::Gains gains;                                   // its curve's
    dsp::LevelFollower reduction;                          // in decibels: rising with the attack, falling with the release
    std::array<dsp::Filter, 2> lowCuts {};                 // the key's, per channel
    std::array<std::array<dsp::Filter, 2>, 2> highCuts {}; // the key's, two sections per channel
    MeterValue levelMeter { -120.0f }, reductionMeter { 0.0f }, boostMeter { 0.0f };
    FallingValue gainReduction { reductionFallPerSecond };
    std::atomic<double> listenUntil { 0.0 }; // in the clock's milliseconds

    // The characters' own state: the average level (RMS) as a mean square, and Opto's release, from the reduction where it
    // began and how long the compressor has held lately (0 to 1).
    float meanSquare = 0.0f, rmsSmoothing = 0.01f;
    float releaseStart = 0.0f, memory = 0.0f, memoryRise = 0.0001f, memoryFall = 0.0001f;
    float opticalCoefficient = 0.0f; // Opto's slow release stage, per sample, at the release set

    // Drive: each channel's last sample into the curve and its antiderivative there (first-order antiderivative
    // anti-aliasing: Parker, Zavalishin and Le Bivic, 2016), and the DC that the even harmonics leave, filtered out.
    std::array<float, 2> lastDriven {}, lastIntegral {};
    std::array<dsp::Filter, 2> dcBlockers {};

    static_assert (std::atomic<double>::is_always_lock_free);
};
} // namespace phazite::devices
