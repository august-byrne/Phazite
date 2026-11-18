// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's saturator (roadmap steps 2.10 and S6.6): drives the sound into a curve that rounds its peaks off, adding
// harmonics that make it warmer, fuller or grittier: soft (like a transistor), tape (gentler and wider), tube (lopsided, so
// it adds even harmonics too), hard (clipping), fold (peaks folded back, for a synthetic edge) or crush (rounded to steps,
// fewer bits the harder it is driven). Colour moves the curve's working point off its middle, as a tube's bias does, which
// adds even harmonics to any curve. Auto gain takes the drive back off its level, so turning it up changes the colour rather
// than the loudness; a tone tilts it darker or brighter afterwards.
//
// With Band on, only the band between From and To goes into the curve, split off by two Linkwitz-Riley crossovers
// (dsp/Crossover.h); the rest passes as it came, as late and turned as the band, and is added back. What the curve bends
// of the band goes through a high-pass at From, so that a band of highs driven hard adds harmonics above it and no
// difference tones under it; Output is then the band's level, which lifts it as an exciter does (the Aural Exciter's side
// chain: a band, a harmonics generator, added back). Listen plays the band alone, as it goes out.
//
// It plays at 2, 4 or 8 times the sample rate (Quality), through polyphase half-band filters, and plays each curve by its
// antiderivative (dsp/Saturation.h), so the harmonics it adds don't fold back down as aliasing.
#pragma once

#include "devices/core/Clock.h"
#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"
#include "devices/dsp/Crossover.h"
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
class Saturator final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteSaturator";

    enum Parameter
    {
        drive,
        shape,
        colour,   // the curve's working point, off its middle: even harmonics
        tone,     // a tilt after the curve
        output,   // the gain after the curve; with Band on, the band's
        autoGain, // takes the drive back off
        band,     // only the band between From and To goes into the curve
        bandLow,  // From: the band's low end; at its lowest, none
        bandHigh, // To: the band's high end, at least an octave above From; at its highest, none
        quality,  // a Quality: how many times the sound's rate the curve plays at
        mix       // applied by its runner (DeviceInfo.h, mixParameterID)
    };

    enum class Shape
    {
        soft,
        tape,
        tube,
        hard,
        fold,
        crush
    };

    enum class Quality
    {
        twice,
        fourTimes,
        eightTimes
    };

    static const DeviceInfo info;

    // The curve as it plays an input (not yet driven), before Auto Gain and Output: driven into the shape around its working
    // point (Colour, 0 to 1), or rounded to the steps the drive gives (Crush). For tests and for drawing it.
    static float playCurve (Shape, float input, float driveDecibels, float colour) noexcept;

    // The band as it plays: From, and To kept at least an octave above it; and whether each has a crossover, or is at the
    // end of its range (none). For its editor and tests.
    struct Band
    {
        float from = 20.0f, to = 20000.0f;
        bool hasLow = false, hasHigh = false;
    };

    static Band getBand (std::span<const float> values) noexcept;

    // How late the sound comes at a Quality, in samples, at any rate.
    static int getLatencyOf (Quality);

    // Message thread: plays the band alone, as it goes out, instead of the sound, for the next half second by the clock; its
    // editor asks again each time it refreshes, so that the sound comes back when the editor no longer shows (as the
    // compressor's Listen). It fades between the two over 5 ms, and its runner plays it whole, whatever the mix.
    void listenToBand() noexcept { listenUntil.store (Clock::getMilliseconds() + listenMilliseconds, std::memory_order_relaxed); }

    bool isPlayingBand() const noexcept { return Clock::getMilliseconds() < listenUntil.load (std::memory_order_relaxed); }

    bool overridesMix() const noexcept override { return listenShare > 0.0f; } // the block it last played

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;

    // Its latency follows the Quality set by hand, as the compressor's follows its mode: automation that moves the Quality
    // while the song plays changes how the curve plays, never how late the sound comes.
    int getLatencySamples() const override { return latency; }
    bool delaysDry() const noexcept override { return true; }
    int getLatencySamplesFor (std::span<const float> valuesSetByHand) const override;
    void takeValuesBeforePrepare (std::span<const float> valuesSetByHand) override;

private:
    static constexpr int numQualities = 3;
    static constexpr double listenMilliseconds = 500.0;

    // A value gliding to where it is set through two one-pole steps, so that it leaves where it was and arrives smoothly,
    // without a corner either end.
    struct Glide
    {
        float value = 0.0f, middle = 0.0f;

        void snap (float to) noexcept { value = middle = to; }
        bool isAt (float target) const noexcept { return juce::exactlyEqual (value, target) && juce::exactlyEqual (middle, target); }
        void moveTo (float target, float step, float closeEnough) noexcept;
    };

    // A fade from 0 to 1 in a straight line, heard along an S (half a cosine), so that it starts and ends without a corner.
    static float getEased (float along) noexcept;

    // What every Quality's way through shares, a sample of the sound's rate at a time: worked out once for the stretch it
    // plays, then read by each way that plays it.
    struct Controls
    {
        std::vector<float> drive, output, bias, slotFade, bandShare, lowShare, highShare, listen, qualityFade, tilt;
        std::array<std::vector<float>, 2> steepness, back, straightGain, dcShare; // each slot's

        void allocate (int length);
    };

    // Where the band's edges move in a stretch: the sample, the edges from there on, and whether an edge comes in from nothing
    // there (its filters start from silence).
    struct Step
    {
        int at = 0;
        float low = 20.0f, high = 20000.0f;
        bool lowStarts = false, highStarts = false;
    };

    // One of the band's edges: a crossover that fades in at a frequency out of hearing and glides to its place, and back,
    // as Safe Bass does (devices/motion/SafeBass.h).
    struct Edge
    {
        bool on = false;
        float off = 5.0f;       // where it fades in and out
        float target = 5.0f;    // where it glides
        float frequency = 5.0f; // where it is
        float amount = 0.0f;    // how far its crossover is in, 0 to 1
        bool starts = false;    // faded in from nothing at the last sample

        void snap() noexcept;
        void move (float fade, float octaves) noexcept; // a sample on
    };

    // One Quality's way through: its filters to the curve's rate and back, the curves and the band's filters there, and
    // the same for the copy of what came in, for the mix.
    struct Path
    {
        int factor = 2, padding = 0, ownLatency = 0, delay = 0;
        double rate = 88200.0;
        std::unique_ptr<juce::dsp::Oversampling<float>> filters, dryFilters;

        struct Channel
        {
            std::array<dsp::AntialiasedCurve, 2> curves; // a shape in each slot
            std::array<dsp::Filter, 2> dcBlockers {};    // what each takes an offset off with
            dsp::AntialiasedCurve straight, outside;     // the band as a straight line would play it, and what is outside it
            dsp::Crossover low, high;                    // at From and To
            dsp::Filter bentHighPass, lowTilt, highTilt;
            std::array<float, 8> padded {};
            dsp::DelayLine late;

            dsp::AntialiasedCurve dryStraight;
            dsp::Crossover dryLow, dryHigh;
            std::array<float, 8> dryPadded {};
            dsp::DelayLine dryLate;
        };

        std::array<Channel, 2> channels;
        std::array<dsp::AntialiasedCurve, 2> points; // each slot's curve fed its working point alone
        std::array<float, 2> pointLevels {};         // what they play
        std::array<int, 2> restedFor {};             // how many samples the working point has stayed where it is
        float lastBias = 0.0f;                       // the working point they last played
        unsigned paddedAt = 0;
        dsp::FilterCoefficients highPass, lowTilt, highTilt, dcBlocker;
        float lastLow = -1.0f, lastHigh = -1.0f, lastTilt = -2.0f;
        bool dryInStep = false; // the copy went through its filters in the last stretch

        void reset() noexcept;
        void startSlot (int index, Shape, float pointValue) noexcept;
        void startSplit (const Step&) noexcept;
        void setStep (const Step&) noexcept;
        void setTilt (float) noexcept;
    };

    double sampleRate = 44100.0;
    int capacity = 1; // the longest stretch it plays at once
    int latency = 0;  // the latency of the Quality set by hand
    Quality preparedQuality = Quality::twice;
    float smoothing = 0.001f; // of the way to new gains, per sample, at each of a glide's two steps
    float fadePerSample = 0.01f, edgeFadePerSample = 0.01f, octavesPerSample = 0.006f, listenStep = 0.01f, dcStep = 0.01f;
    int quickWarmUp = 1, slowWarmUp = 1; // samples a shape or a Quality plays unheard before it is faded to: an even curve's,
                                         // and one whose offset its DC blocker must settle on
    bool isFirstBlock = true;
    float lastMix = 1.0f; // the mix in its last block

    // Drive in decibels, the share of Auto Gain, Output as a gain, Colour's working point and the tone, gliding.
    Glide driveDecibels, autoShare, outputGain, bias, tilt;
    float driveGain = 1.0f; // from driveDecibels

    std::array<Shape, 2> slotShapes { Shape::soft, Shape::soft };
    int slot = 0;            // the slot that plays; the other warms up, then fades in
    bool slotFading = false; // the other slot plays beside it
    int slotWarmed = 0, slotWarmUp = 1;
    float slotFade = 0.0f;
    std::array<float, 2> dcShares {}; // how far each slot's DC blocker is in

    float bandShare = 0.0f; // how far the sound goes the band's way, 0 to 1
    bool splitting = false; // the band's filters play
    Edge low, high;
    float listenShare = 0.0f;
    std::atomic<double> listenUntil { 0.0 }; // in the clock's milliseconds

    std::array<Path, numQualities> paths;
    int active = 0, next = -1;         // the Quality that plays, and one warming up or faded to
    int warmed = 0, qualityWarmUp = 1; // samples the next has played, and how many it plays unheard
    float qualityFade = 0.0f;

    Controls controls;
    std::vector<Step> steps;
    int numSteps = 0;
    juce::AudioBuffer<float> spare, spareDry; // what came in, for the Quality warming up

    static_assert (std::atomic<double>::is_always_lock_free);

    void play (float* const* channels, float* const* dry, int numChannels, int length, std::span<const float> values, bool usesDry);
    void workOutControls (int length, std::span<const float> values);
    void playPath (Path&, float* const* channels, float* const* dry, int numChannels, int length, bool usesDry);
    int getPlayable (int wanted) const noexcept;
    bool isLopsided (int index, float targetBias) const noexcept; // a slot's curve, which leaves an offset
};
} // namespace phazite::devices
