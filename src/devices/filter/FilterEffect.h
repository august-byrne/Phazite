// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's filter (roadmap step 2.10), the "Filter" device, with a drive that saturates what goes into it: one of the
// filter types every Phazite filter offers (dsp/MultimodeFilter.h), a resonant low-pass, high-pass, band-pass or notch,
// 12 or 24 dB per octave, to sweep a sound open or closed; Comb + and Comb -, whose cutoff sets the pitch of their peaks
// and whose resonance makes them ring; or Vowel, Phaser or Sample & Hold. Its cutoff glides rather than jumps, so
// automation, modulators and a turned knob sweep it smoothly. A new filter changes little: a low-pass open all the way.
// The drive runs at twice the sample rate, its curve played by its antiderivative (dsp/Saturation.h), so that what it
// adds doesn't fold back as aliasing; undriven, the sound takes a plain delay as long instead, so that its latency stays
// the same, faded from one to the other as the drive comes on or off.
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
#include <vector>

namespace phazite::devices
{
class FilterEffect final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteFilter";

    enum Parameter
    {
        filterType, // a dsp::FilterMode
        cutoff,
        resonance,
        filterSlope,
        drive,
        mix // applied by its runner (DeviceInfo.h, mixParameterID)
    };

    static const DeviceInfo info;

    // Its response at a frequency for the parameters' values (a gain, not in decibels): for its editor and tests.
    static double getMagnitude (std::span<const float> parameters, double frequency, double sampleRate);

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    double getTailSeconds() const override; // a comb's ring
    int getLatencySamples() const override { return latency; }
    bool delaysDry() const noexcept override { return true; }

private:
    static constexpr int coefficientStep = 8; // samples between new coefficients while the cutoff glides

    double sampleRate = 44100.0;
    int latency = 0;
    float glide = 0.001f;  // of the way to a new cutoff, per sample (in octaves)
    float octaves = 10.0f; // the cutoff now, in octaves above 20 Hz

    // The coefficients step on the song's own steps of coefficientStep samples: the samples left of the step under way,
    // which a block's end cut (it goes on in the next block with the coefficients it began with), and the song's sample the
    // next block goes on from (any other, a jump or the song stopped, starts a step afresh).
    int stepLeft = 0;
    std::int64_t nextSongSample = -1;
    float driveGain = 1.0f; // smoothed, at twice the rate
    float driveSmoothing = 0.001f;
    bool isFirstBlock = true, wasDriven = false;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    std::array<dsp::AntialiasedCurve, 2> curves;
    std::array<dsp::DelayLine, 2> undriven;        // the sound as late as the drive makes it
    std::array<std::vector<float>, 2> undrivenOut; // a block of it, to fade from and to
    float driveBlend = 0.0f;                       // how far the sound goes through the drive
    std::array<dsp::MultimodeFilter, 2> filters;   // [channel]
    std::atomic<float> tailSeconds { 0.0f };

    // The copy of what came in for its mix (ProcessContext::dry): as late as the sound through a plain delay, and, while
    // the sound goes through the drive and the mix uses the copy, through filters like the drive's with a straight line in
    // the curve's place, faded as the sound is: the half-band filters turn the phase of the top octave, and the copy is
    // turned as the sound is, so that the two meet in phase at any mix.
    std::unique_ptr<juce::dsp::Oversampling<float>> dryOversampling;
    std::array<dsp::AntialiasedCurve, 2> dryCurves;
    std::array<dsp::DelayLine, 2> dryUndriven;
    std::array<std::vector<float>, 2> dryUndrivenOut;
    float lastMix = 1.0f;   // the mix in its last block
    bool dryInStep = false; // the copy went through its filters in the last block

    void driveBlock (const ProcessContext&, int numChannels, float targetDrive) noexcept;
    void dryBlock (const ProcessContext&, int numChannels, int numSamples, bool throughDrive, float blendFrom, float blendTarget) noexcept;
};
} // namespace phazite::devices
