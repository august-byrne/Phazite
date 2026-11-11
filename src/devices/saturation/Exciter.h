// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's exciter: takes the highs above a frequency, drives them into a curve that makes harmonics of them, and adds
// what comes out to the sound, which itself passes untouched: the air, presence and bite that an equalizer can't give a
// dull sound, since it can only raise what is there. As the Aphex Aural Exciter does it (a side chain: a high-pass, a
// harmonics generator, mixed back in), and as Logic's Exciter and Calf's do after it.
//
// - Frequency is where the band starts: a high-pass of 24 dB an octave (two Butterworth sections) before the curve, so that
//   the notes' fundamentals stay out of it, and the same again after it, so that only what was made above the band is
//   added (the difference tones a curve makes below it stay out). High Cut ends the band, the same way, for a top that
//   mustn't fizz; at 20 kHz it is off.
// - Drive is how hard the band is pushed into the curve, a soft clip (softClip, dsp/Math.h), and comes back off after it:
//   what the curve leaves straight is added at Amount, whatever the drive, and what reaches its bend is held down and
//   turned into harmonics. So Drive sets the level from which the band is squeezed into harmonics, and never the loudness.
// - Colour moves the curve's working point off its middle, as a tube's bias does: at 0 the curve is even-sided and makes
//   odd harmonics alone (the third first: edgy); turned up it is lopsided and makes even ones too (the second first:
//   sweeter), until at 100 % the third is gone from quiet sounds and the second leads.
// - Amount is how much of the excited band is added: at 100 % as loud as the band came in, which lifts the top by 6 dB.
//
// The added band sums with the sound in phase at every frequency, so that it lifts the top as a shelf does and never
// combs: a high-pass steep enough to keep the fundamentals out turns the phase by half a cycle or more around its
// frequency, and added to the sound it would cut a dip there (Calf's dips at 1.93 times its frequency, where its four
// high-pass sections turn the band half a cycle). So the filter after the curve is the one before it played backwards (its
// impulse response reversed, as a filter of 2 ms, as filtering forward and then backward does offline): the two together
// turn no phase at all, and pass the band with the square of one's gain, 48 dB an octave. The sound waits those 2 ms for
// it, and for the curve: the device's latency, 3.3 ms at 44.1 and 48 kHz, 2.3 ms at 192 kHz. The curve plays at four times
// the rate (twice from 88.2 kHz on) by its antiderivative (dsp/Saturation.h), through linear-phase filters, and only what
// it bends goes that way: what it leaves straight is added as it is, exactly in step.
//
// Listen plays what is added alone, for setting it by ear, as every exciter lets one (its editor's switch, not a
// parameter: a song can't be saved with it on).
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/MeterValue.h"
#include "devices/core/Processor.h"
#include "devices/core/SampleTap.h"
#include "devices/dsp/DelayLine.h"
#include "devices/dsp/Filters.h"
#include "devices/dsp/Saturation.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <memory>
#include <span>
#include <vector>

namespace phazite::devices
{
class Exciter final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteExciter";

    enum Parameter
    {
        frequency, // where the band starts
        drive,     // into the curve, in decibels
        colour,    // from odd harmonics alone to even ones too
        amount,    // how much of the excited band is added
        highCut,   // where the band ends; at its highest, off
        mix        // applied by its runner (DeviceInfo.h, mixParameterID)
    };

    static const DeviceInfo info;

    // The curve as the band meets it, for a sample already driven, at a colour (0 to 1): 0 for 0, and as steep there as a
    // straight line, whatever the colour. For tests and for drawing it.
    static float shapeSample (float colour, float driven) noexcept;

    // How much of the band is added at a frequency while the curve leaves it straight, as a gain from 0 to 1 (without
    // Amount): the square of the band's filters' gain. And what that makes of the sound there, with Amount, in decibels: its
    // lift. For its editor and tests.
    static double getBandGain (std::span<const float> parameters, double frequency, double sampleRate);
    static double getLift (std::span<const float> parameters, double frequency, double sampleRate);

    // Message thread: plays what is added alone, instead of the sound with it, for the next half second by the clock; its
    // editor asks again each time it refreshes, so that the sound comes back when the editor no longer shows (as the
    // compressor's Listen). It fades between the two over a few milliseconds, and its runner plays it whole, whatever the mix.
    void listenToAdded() noexcept
    {
        listenUntil.store (juce::Time::getMillisecondCounterHiRes() + listenMilliseconds, std::memory_order_relaxed);
    }

    bool isPlayingAdded() const noexcept { return juce::Time::getMillisecondCounterHiRes() < listenUntil.load (std::memory_order_relaxed); }

    bool overridesMix() const noexcept override { return addedShare > 0.0f; } // the block it last played

    // For its editor: what is added, both channels averaged (while a display listens), and the highest the driven band
    // reached into the curve since it last looked: the curve bends from about 0.3 on, and is flat from 3.
    SampleTap& getAddedTap() noexcept { return addedTap; }
    MeterValue& getReach() noexcept { return reach; }

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    int getLatencySamples() const override { return latency; }

private:
    // A channel: the band's filters before the curve, the curve and a straight line beside it (what the curve bends is the
    // difference of the two), the band and the sound waiting for what comes back, and the last of the excited band for the
    // filter after the curve.
    struct Channel
    {
        std::array<dsp::Filter, 2> highPasses {}, lowPasses {};
        dsp::AntialiasedCurve curve, straight;
        std::array<float, 4> padded {}; // the last of what the curve made, for the samples that make its way whole
        dsp::DelayLine band, sound;
        std::vector<float> history; // the last of the excited band twice over, so that they are always in a row
        int newest = 0;             // where the newest of them is
    };

    double sampleRate = 44100.0, curveRate = 176400.0;
    int blockCapacity = 1; // the longest stretch it plays at once
    int latency = 0;
    int curveLatency = 0;     // how late what the curve bends comes back, in samples
    int padding = 0;          // the samples at the curve's rate that make that a whole number, 3 at most
    unsigned paddedAt = 0;    // where the newest of them is, in each channel's
    int reverseLength = 1;    // the filter after the curve: how many samples it weighs, one more than it delays
    int usedTaps = 1;         // how many of them aren't nothing, with the band where it is
    float smoothing = 0.001f; // of the way to a new drive or colour, per sample at the curve's rate
    float amountSmoothing = 0.001f, bandSmoothing = 0.01f; // to a new amount, and to the band's new edges, per sample
    float driveGain = 1.0f, bias = 0.0f, amountGain = 0.0f;
    float steepness = 1.0f, restLevel = 0.0f; // the curve at its working point: one over its slope, and its level, as played
    int restedFor = 0;                        // how many samples the working point has stayed where it is
    dsp::AntialiasedCurve rest;               // the curve playing its working point alone
    float lastFrequency = -1.0f, lastHighCut = -1.0f;
    bool cutsHighs = false;
    bool isFirstBlock = true;
    float cutShare = 0.0f, cutStep = 0.01f;    // how much of the band is through High Cut, as it turns on or off, and its step
    dsp::FilterCoefficients highPass, lowPass; // where the band's edges are going,
    dsp::FilterCoefficients nowHigh, nowLow;   // and where they are, gliding there
    std::vector<float> taps;                   // the filters before the curve, as they answer an impulse
    std::vector<float> band, excited;          // a stretch of the band, and of what the curve bent of it, each channel's in a row
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    std::array<Channel, 2> channels;

    static constexpr double listenMilliseconds = 500.0;
    float addedShare = 0.0f, shareStep = 0.01f; // how much of the sound gives way to what is added alone, and its step a sample
    std::atomic<double> listenUntil { 0.0 };    // in the clock's milliseconds
    SampleTap addedTap;
    MeterValue reach { 0.0f };

    static_assert (std::atomic<double>::is_always_lock_free);

    void setBand (float frequency, float highCut);
};
} // namespace phazite::devices
