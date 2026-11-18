// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's filter (roadmap steps 2.10 and S6.6), the "Filter" device, with a drive: one of the filter types every Phazite
// filter offers (dsp/MultimodeFilter.h), a resonant low-pass, high-pass, band-pass or notch, 12 or 24 dB per octave, to sweep
// a sound open or closed; Comb + and Comb -, whose cutoff sets the pitch of their peaks and whose resonance makes them ring;
// or Vowel, Phaser or Sample & Hold. Its cutoff glides rather than jumps, so automation, modulators and a turned knob sweep it
// smoothly. A new filter changes little: a low-pass open all the way.
//
// Its Character is Clean or Driven. Clean: the drive saturates what goes into the filter and gives back the gain it adds;
// the filter is linear. Driven (low-pass, high-pass, band-pass and notch; the other types play Clean): the drive pushes the
// sound into the filter's loop, where a curve bends the resonance (dsp::DrivenCoefficients), so that loud sound tames the
// peak, the top of the Resonance sings on its own, and what comes out is half as much louder as the drive (in decibels),
// through a ceiling at full scale that leaves a sound under -6 dBFS alone (dsp::KneeCurve). A Driven filter never sleeps on
// silence: it can start to sing by itself.
//
// Its Quality: the drive, and the Driven filter, play at 2, 4 or 8 times the sample rate through polyphase half-band
// filters, each curve played by its antiderivative (dsp/Saturation.h), so that what they add doesn't fold back as aliasing.
// Every Quality takes the latest's latency, the others waiting, so that automation can move it anywhere. A change of
// Character or Quality starts the new way unheard, lets it settle, and crossfades to it over 10 ms; a change meanwhile waits
// for that to end.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"
#include "devices/dsp/DelayLine.h"
#include "devices/dsp/MultimodeFilter.h"
#include "devices/dsp/Saturation.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace phazite::devices
{
class FilterEffect final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteFilter";
    static constexpr double longestTailSeconds = 60.0; // the tail it reports while it sings on its own

    enum Parameter
    {
        filterType, // a dsp::FilterMode
        cutoff,
        resonance,
        filterSlope,
        character, // a Character
        drive,
        quality, // a Quality: how many times the sound's rate the drive and the Driven filter play at
        mix      // applied by its runner (DeviceInfo.h, mixParameterID)
    };

    enum class Character
    {
        clean,
        driven
    };

    enum class Quality
    {
        twice,
        fourTimes,
        eightTimes
    };

    static const DeviceInfo info;

    // Its response at a frequency for the parameters' values (a gain, not in decibels): Driven's for a quiet sound, whose peak
    // goes off any scale where it sings. For its editor and tests.
    static double getMagnitude (std::span<const float> parameters, double frequency, double sampleRate);

    // Whether the values play the Driven filter: Character on Driven, and a type that has it.
    static bool isDriven (std::span<const float> parameters) noexcept;

    // How late the sound comes, in samples, at any rate and every Quality: the latest Quality's, which the others wait for.
    static int getLatency();

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    double getTailSeconds() const override; // a comb's ring, and the Driven filter's
    int getLatencySamples() const override { return latency; }
    bool delaysDry() const noexcept override { return true; }

    // Not while the values it played last were Driven's (Character on Driven, a type that has it): a Driven filter at rest
    // can start to sing as its Resonance rises, which a sleeping one wouldn't hear. Any thread.
    bool sleepsOnSilence() const noexcept override;

    // A Clean filter asleep that its values make Driven at a Resonance that sings would sing from silence: woken for them,
    // and for no others (a Clean filter makes no sound of its own, whatever moves).
    bool wakesWhenValuesChange() const noexcept override { return true; }
    bool wakesFor (std::span<const float> values) const noexcept override { return isDriven (values); }

private:
    static constexpr int numQualities = 3;
    static constexpr int numWays = 2 * numQualities; // Clean at each Quality, then Driven at each
    static constexpr int coefficientStep = 8;        // samples between new coefficients while the cutoff glides

    // Where the coefficients change in a stretch: the sample, how long the step is, the cutoff (in octaves above 20 Hz)
    // from there, and whether it glided there.
    struct Step
    {
        int at = 0, length = 0;
        float octaves = 0.0f;
        bool moved = false;
    };

    // The drive before the Clean filter, at a Quality: the sound times the drive's gain, soft-clipped and divided by it again;
    // faded from the plain delay as the drive comes on, and back as it goes off.
    struct CleanStage
    {
        std::unique_ptr<juce::dsp::Oversampling<float>> filters;
        std::array<dsp::AntialiasedCurve, 2> curves;
        float driveGain = 1.0f;  // smoothed, at its rate
        float driveBlend = 0.0f; // how far the sound goes through the drive
        bool wasDriven = false;
    };

    // The Driven filter at a Quality: the sound times the drive's gain into the loop, half the gain given back, and a
    // ceiling (dsp::KneeCurve).
    struct DrivenStage
    {
        std::unique_ptr<juce::dsp::Oversampling<float>> filters;
        std::array<dsp::MultimodeFilter, 2> loops;
        std::array<dsp::AntialiasedCurve, 2> ceilings;
        dsp::DrivenCoefficients coefficients;
        float madeOctaves = -1.0f, madeType = -1.0f, madeResonance = -1.0f, madeSlope = -1.0f; // what they were made for
        float driveGain = 1.0f;                                                                // smoothed, at its rate
        float giveBack = 1.0f;                                                                 // 1 / sqrt (driveGain)
    };

    // One Quality's filters to its rate and back, and what it needs: a Clean drive, a Driven filter, and the copy of what came
    // in for the mix through the same half-band filters with a straight line in the curve's place. Each comes ownLatency
    // late (the padding makes the curve's delay whole samples), then waits `delay` more in a plain delay, as late as every
    // Quality.
    struct Path
    {
        int factor = 2, padding = 0, ownLatency = 0, delay = 0;
        double rate = 88200.0;
        float smoothing = 0.001f; // of the way to a new drive, per sample at its rate
        CleanStage clean;
        DrivenStage driven;
        std::unique_ptr<juce::dsp::Oversampling<float>> dryFilters;
        std::array<dsp::AntialiasedCurve, 2> dryStraight;
        bool dryInStep = false; // the copy went through its filters in the last stretch
        bool dryNeeded = false; // and goes through them in this one

        // [channel]: the padding's samples, and the plain delays after the Clean drive, its copy, the Driven filter and its.
        std::array<std::array<float, 8>, 2> cleanPadded {}, drivenPadded {}, dryPadded {};
        unsigned cleanPaddedAt = 0, drivenPaddedAt = 0, dryPaddedAt = 0;
        std::array<dsp::DelayLine, 2> cleanLate, cleanDryLate, drivenLate, drivenDryLate;

        // What each played in the stretch, [channel][sample]: the Clean drive and its copy, the Driven filter and its copy,
        // the drive alone (before its blend with the plain delay) and the copy through the half-band filters; and the Clean
        // drive's blend at each sample.
        juce::AudioBuffer<float> cleanOut, cleanDryOut, drivenOut, drivenDryOut, scratch, dryOut;
        std::vector<float> blends;
    };

    double sampleRate = 44100.0;
    int capacity = 1;      // the longest stretch it plays at once
    int latency = 0;       // the latest Quality's, which every Quality takes
    float glide = 0.001f;  // of the way to a new cutoff, per sample (in octaves)
    float octaves = 10.0f; // the cutoff now, in octaves above 20 Hz
    float fadePerSample = 0.01f;
    int warmUpLeast = 256;
    bool isFirstBlock = true;
    float lastMix = 1.0f; // the mix in its last block

    // The coefficients step on the song's own steps of coefficientStep samples: the samples left of the step under way,
    // which a block's end cut (it goes on in the next block with the coefficients it began with), and the song's sample the
    // next block goes on from (any other, a jump or the song stopped, starts a step afresh).
    int stepLeft = 0;
    std::int64_t nextSongSample = -1;
    std::vector<Step> steps;
    int numSteps = 0;

    std::array<Path, numQualities> paths;
    std::array<dsp::MultimodeFilter, 2> filters;         // [channel]: the Clean filter, at the sound's rate
    std::array<dsp::DelayLine, 2> undriven, dryUndriven; // the sound and its copy, plain, read as late as each way is
    juce::AudioBuffer<float> undrivenOut, cleanIn;       // the sound as late as every Quality; what the Clean filter plays

    // The way that plays (Clean or Driven at a Quality: Path index, plus numQualities for Driven), and one warming up beside
    // it, then faded to (-1: none): how many samples it has warmed and is to, and how far it is faded to.
    int active = 0, next = -1;
    int warmed = 0, warmUp = 256;
    float fade = 0.0f;
    std::vector<float> fades; // eased, at each sample of the stretch
    std::atomic<float> tailSeconds { 0.0f };
    std::atomic<bool> playedDriven { false }; // whether the values of its last block were Driven's

    static bool isDrivenWay (int way) noexcept { return way >= numQualities; }

    // Plays a stretch from a block's sample `start`, at most `most` long, ending where a fade ends; returns how long.
    int play (const ProcessContext&, int start, int most, bool usesDry);
    int workOutFades (int most) noexcept;
    void workOutSteps (const ProcessContext&, int start, int length, float targetOctaves) noexcept;
    void startWay (int way, std::span<const float> values, float targetDrive) noexcept;
    void makeDrivenCoefficients (Path&, std::span<const float> values, float atOctaves) noexcept;
    void playClean (Path&, const ProcessContext&, int start, int length, float targetDrive, bool turned) noexcept;
    void playDriven (Path&, const ProcessContext&, int start, int length, float targetDrive, bool usesDry) noexcept;
    void playDry (Path&, const ProcessContext&, int start, int length) noexcept;
    void playCleanFilter (std::span<const float> values, int length, float startOctaves, int numChannels) noexcept;
    static void late (const Path&, juce::AudioBuffer<float>&, std::array<dsp::DelayLine, 2>&, int numChannels, int length) noexcept;
};
} // namespace phazite::devices
