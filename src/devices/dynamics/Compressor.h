// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's compressor (roadmap step 1.6): it turns the sound down by the ratio as far as its level goes over the
// threshold, easing in across a soft knee, with an attack, a release and makeup gain. Its character sets how it hears and
// reacts, as Logic's circuit types and FabFilter's styles do (the owner, 2026-11-01: "depth with different compression
// types"): clean, as set and without colour of its own; VCA, which hears the average level, so short peaks pass; FET,
// which listens to what it puts out and grips fast; Opto, which lets go fast at first and then slowly, the longer it was
// held the slower; and Tube, whose ratio grows with the level. Drive saturates what it puts out in the character's colour
// (FET's odd harmonics, Opto's and Tube's even ones, Clean's and VCA's soft clipping), without aliasing much
// (antiderivative anti-aliasing), so a tube compressor's warmth is one device. Its detector listens to the sound itself,
// or to a sidechain (another track, for ducking): its key, through a low cut so that bass notes don't pump it, and a high
// cut (off at first) so that a kick ducks a bass and the hats on the same track don't; and it turns both channels down
// alike, so that the stereo image holds. Its meters show how loud the detector hears the key and how far the compressor
// turns the sound down; while its editor asks, it plays the key itself, as its cuts leave it, to hear what it listens to.
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
        character, // a Character
        inputGain,
        threshold,
        ratio,
        attack,
        release,
        knee,
        makeup,
        drive, // from 0 (none) to 1
        keyLowCut,
        keyHighCut,
        mix // applied by its thin plug-in (DeviceInfo.h, mixParameterID)
    };

    enum class Character
    {
        clean, // hears each peak and reacts as set, with no colour of its own
        vca,   // hears the average (RMS) level, so short peaks pass: punchy, for buses
        fet,   // hears what it puts out (feedback) and grips fast: aggressive; drive adds odd harmonics
        opto,  // feedback, with a soft knee and a release that slows as it goes, the more the longer it held
        tube   // feedback, its ratio growing from none at the threshold to the one set 15 dB over; drive adds even harmonics
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
    float makeupDecibels = 0.0f, inputDecibels = 0.0f, driveAmount = 0.0f;
    float lastInputDecibels = 0.0f, lastInput = 1.0f;             // the input gain last worked out
    float lastGainDecibels = 0.0f, lastGain = 1.0f;               // the gain last worked out
    float lastReductionDecibels = 0.0f, lastReductionGain = 1.0f; // without the makeup, for a detector that listens to it
    float lastDriveAmount = -1.0f, driveGain = 1.0f;              // into drive's curve
    float lastMakeupDecibels = 0.0f, makeupGain = 1.0f;           // after it
    bool wasDriven = false;
    bool isFirstBlock = true;
    static constexpr double listenMilliseconds = 500.0;
    float keyShare = 0.0f, keyStep = 0.01f; // how much of the key it plays, from 0 to 1, and its step per sample

    dsp::LevelFollower reduction;                          // in decibels: rising with the attack, falling with the release
    std::array<dsp::Filter, 2> lowCuts {};                 // the key's, per channel
    std::array<std::array<dsp::Filter, 2>, 2> highCuts {}; // the key's, two sections per channel
    MeterValue levelMeter { -120.0f }, reductionMeter { 0.0f };
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
