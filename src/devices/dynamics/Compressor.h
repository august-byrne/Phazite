// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's compressor (roadmap step 1.6), in one of two modes, as Fruity Limiter's tabs switch its own (the owner,
// 2026-11-11: "should the line graph even co-exist in the same effect as sidechaining"; docs/design.md, section 9): Compress
// and Duck.
//
// Compress: it turns each level of its own sound as its curve says, drawn as Maximus draws its curves (DynamicsCurve.h; the
// owner, 2026-11-01: "i want this graph to also be used the same in the compressor plugin"), so that it compresses or
// expands, downward or upward, limits or gates, by the curve's shape alone, with an attack (as the gain goes down), a release
// (as it comes back up) and input and makeup gains around it. Its character sets how it hears and reacts, as Logic's circuit
// types and FabFilter's styles do (the owner, 2026-11-01: "depth with different compression types"): Clean, which hears each
// peak and adds no colour of its own; VCA, which hears the average level, so short peaks pass; FET, which hears each peak and
// adds odd harmonics the harder it turns down, as an 1176 does; Opto, which hears the average and lets go fast at first and
// then slowly, the longer it held the slower; and Tube, which hears the average and adds even harmonics the harder it turns
// down. Each hears what comes in (feed-forward), so that once settled it follows its curve exactly whatever its character; a
// compressor that listened to what it put out would settle elsewhere than the curve drawn, and run away where the curve
// expands. Drive saturates what it puts out in the character's colour (FET's odd harmonics, Opto's and Tube's even ones,
// Clean's and VCA's soft clipping), without aliasing much (antiderivative anti-aliasing). It hears its own sound alone,
// through the key's low cut so that bass notes don't pump it, as a bus compressor's sidechain high-pass does (the high cut is
// Duck's): a sidechain it is given isn't heard while it compresses, kept for when it ducks again.
//
// Duck: it turns the sound down under its key (the track its sidechain listens to, or its own sound without one), as
// Kickstart, ShaperBox and Duck do, with no curve: as far as its depth while the key, as its cuts leave it, is 6 dB or more
// over its threshold (less, the less it is over), reached in a straight line in decibels over its attack; held there for at
// least its hold from the hit, and for as long as the key stays over the threshold, so that a short kick still gives a duck
// of a set length and a held note keeps it down (FabFilter Pro-C 3: "With longer hold times, you can achieve nice pumping
// effects"); then back over its release, straight in decibels or bent by its release curve (slow at first, the sound staying
// down and springing back; or quick at first), as Fruity Limiter's CURVE bends its own. A new hit during the release grips
// again from where it is. Its curve, character, input gain, drive and makeup do nothing while it ducks.
//
// In both, it turns both channels alike, so that the stereo image holds. Its meters show how loud the detector hears (its
// own sound, or the key) and how far it turns the sound down and up; while its editor asks, it plays what its detector hears
// itself, as its cuts leave it, to hear what it listens to.
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
        mode,      // a Mode
        character, // a Character: Compress's
        inputGain, // Compress's
        threshold, // Duck's, in decibels: the key's level that starts it
        depth,     // Duck's: how far it turns the sound down, in decibels
        attack,
        hold, // Duck's: the least it stays down from a hit, in milliseconds
        release,
        releaseCurve, // Duck's: from -1 (quick at first) through 0 (straight in decibels) to 1 (slow at first)
        makeup,       // Compress's
        drive,        // Compress's: from 0 (none) to 1
        keyLowCut,
        keyHighCut,
        points,                               // Compress's curve (DynamicsCurve.h): how many points it has, then its points
        mix = points + curves::parameterCount // applied by its runner (DeviceInfo.h, mixParameterID)
    };

    enum class Mode
    {
        compress, // its own sound, by its curve
        duck      // under its key, by its threshold, depth, attack, hold and release
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

    // Its mode, from its parameter's value.
    static Mode getMode (float value) noexcept { return value >= 0.5f ? Mode::duck : Mode::compress; }

    // How long the detector holds the key's loudest level while it ducks, in milliseconds: at least half a cycle of a kick's
    // lowest notes (a 50 Hz kick's is 10 ms), so that the level it hears doesn't fall to nothing as the key's wave crosses 0,
    // which would let the sound go between the cycles of one kick.
    static constexpr int keyPeakMilliseconds = 10;

    // The loudest the detector heard, in decibels, and the most the compressor turned the sound down by and up by, in
    // decibels (0 or more), since its editor last looked (devices/core/MeterValue.h).
    MeterValue& getLevelMeter() noexcept { return levelMeter; }
    MeterValue& getReductionMeter() noexcept { return reductionMeter; }
    MeterValue& getBoostMeter() noexcept { return boostMeter; }

    // Message thread: plays the key, as the detector hears it, instead of the sound, for the next half second by the clock;
    // its editor asks again each time it refreshes, so that the key stops when the editor no longer shows, whether the
    // device played meanwhile or not. It fades between the two over a few milliseconds, and its runner plays it whole,
    // whatever the mix.
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
    void compress (const ProcessContext&);
    void duck (const ProcessContext&);

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

    // Duck's: the key's loudest over the last keyPeakMilliseconds, a millisecond at a time (each finished millisecond's
    // loudest, and the one going on now's); how far it turns the sound down now, in decibels; the samples since the last hit,
    // the key rising over the threshold (counted up to the most a hold can be); and the release going on: where it started,
    // and how far along it is (0 to 1).
    std::array<float, keyPeakMilliseconds> keyPeaks {};
    int keyPeakIndex = 0, keyPeakSamples = 0, samplesPerPeak = 48;
    float keyPeakNow = 0.0f, keyPeakBefore = 0.0f; // the millisecond going on, and the loudest of those before it
    float lastKeyPeak = -1.0f, keyDecibels = -120.0f;
    float ducked = 0.0f, lastDucked = -1.0f, duckGain = 1.0f;
    int sinceHit = 0;
    float releaseFrom = 0.0f;
    double releaseAlong = 0.0;
    bool releasing = false, wasOver = false;

    static_assert (std::atomic<double>::is_always_lock_free);
};
} // namespace phazite::devices
