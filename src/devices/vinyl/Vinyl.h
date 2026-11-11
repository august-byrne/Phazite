// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's vinyl: what a record and its player do to a sound, as producers of lo-fi, hip-hop and synthwave reach for one to
// do (iZotope's Vinyl, XLN's RC-20, Ableton's Vinyl Distortion), in the order a record makes it:
//
// - Cut: the lows below Bass Mono summed to mono, as a cutting engineer sums them so that the stylus stays in the groove (at
//   its lowest, 20 Hz, off). The side's lows are taken off by a Linkwitz-Riley split, the middle kept in phase with it.
// - Wow: the slow swing of pitch of a record that is warped or off its centre, once a turn: Wow is how far, in cents, and Wow
//   Rate how fast the record turns (0.56 Hz is 33 1/3 rpm, 0.75 Hz is 45 and 1.3 Hz is 78). A warp swings in more than one
//   hump, so a quarter as much again comes at twice the rate, the record flat for longer than sharp (-1.25 to +0.75 times
//   the depth). Flutter is the drive's quicker, unsteady wobble: noise below 6 Hz, its depth in cents RMS (times the square
//   root of two, as a sine's). Both move the sound through a delay line read at twice the rate by Lagrange interpolation
//   (DelayLine.h), so the top keeps its level as the delay moves.
// - The groove's distortion: as a stylus's round tip traces a groove that a sharp chisel cut, the loud highs bend, more the
//   louder and higher they are (the groove's curves are tighter), and the bass hardly at all. Distortion drives what is above
//   2 kHz (a high-pass of 12 dB an octave) into a lopsided curve, a second harmonic first and a third as it is pushed, and
//   adds what the curve bends to the sound, which itself passes untouched: at 0 % there is no curve at all, and at 100 % the
//   highs are pushed 12 dB into it. The curve plays at twice the rate by its antiderivative (dsp/Saturation.h), what it adds
//   kept apart from the sound and added back in step with it.
// - Surface noise (SurfaceNoise.h): Hiss, the groove's hush, its RMS; Crackle, the largest clicks' peak, and Density, how
//   many come a second. At their lowest, off. They play while the song plays, from wherever it starts, and fade out over
//   20 ms when it stops, as a record lifts off, and 2 s after the sound coming in has stopped, as a run-out groove ends, so
//   that an export ends with the song rather than with ten seconds of crackle.
// - The player: Width, how far apart its two sides are (100 % as the record has them, 0 % mono), and its band, Low Cut and
//   High Cut, Butterworth filters of 12 dB an octave that roll the record's ends off, the noise's too.
//
// Every move glides: the wow's depth and rate over 50 ms, and how far they swing the delay no faster than would bend the
// pitch 7 cents, so that a deep wow turned up never swoops; the flutter's depth over 300 ms, the band's edges over 5 ms, the
// rest over 20 ms. The delay line's middle is the device's latency: as deep a wow as the slowest rate allows, and the
// flutter's deepest, with the oversampling's few samples; about 11 ms. Its runner applies the mix, with what came in as late.
//
// Its editor reads the record's turn, the pitch now and its history, and the clicks as they come. Real-time safe: nothing
// is allocated after prepare(), and while the song is stopped and nothing comes in it rests, after its delay line has
// emptied, until sound or the song comes again (so its runner never needs to put it to sleep, which would keep its noise
// from starting at a silent song's start).
#pragma once

#include "SurfaceNoise.h"
#include "devices/core/DeviceInfo.h"
#include "devices/core/LiveValue.h"
#include "devices/core/Processor.h"
#include "devices/core/SampleTap.h"
#include "devices/dsp/Crossover.h"
#include "devices/dsp/DelayLine.h"
#include "devices/dsp/Filters.h"
#include "devices/dsp/Oversampling.h"
#include "devices/dsp/Random.h"
#include "devices/dsp/Saturation.h"

#include <array>
#include <span>

namespace phazite::devices
{
class Vinyl final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteVinyl";

    enum Parameter
    {
        wow,        // cents, once a turn
        wowRate,    // hertz: the record's turns a second
        flutter,    // cents RMS, times the square root of two
        lowCut,     // the player's band, from
        highCut,    // and to
        distortion, // the groove's, from 0 (none) to 1
        width,      // the two sides apart, from 0 (mono) to 1 (as they come)
        bassMono,   // below it, the lows in mono; at its lowest, off
        hiss,       // decibels RMS; at its lowest, off
        crackle,    // decibels, the largest clicks' peak; at its lowest, off
        density,    // clicks a second, on average
        mix         // applied by its runner (DeviceInfo.h, mixParameterID)
    };

    static const DeviceInfo info;

    // Whether a level parameter (Hiss, Crackle) or Bass Mono is at its lowest, which turns it off.
    static bool isOff (std::span<const float> parameters, Parameter);

    // The pitch the wow plays a part of the way through a turn (0 to 1), in cents: for its editor, which draws it, and tests.
    static float getWowCents (float wowCents, float turn) noexcept;

    // What the band does to a frequency, the Low Cut's and the High Cut's gain together (not in decibels): for its editor.
    static double getBandGain (std::span<const float> parameters, double frequency, double sampleRate);

    // The groove's curve as it bends a sample of the highs, at a distortion (0 to 1): the sample itself at 0. For tests and for
    // drawing it.
    static float shapeSample (float distortion, float sample) noexcept;

    // For its editor: how far through its turn the record is (0 to 1); the pitch it plays now, in cents; its pitch over the
    // last moments, one reading every 10 ms (while a display listens); and its noise, for the clicks that came and the largest.
    const LiveValue& getTurn() const noexcept { return turn; }
    const LiveValue& getPitch() const noexcept { return pitchNow; }
    SampleTap& getPitchTap() noexcept { return pitchTap; }
    SurfaceNoise& getSurfaceNoise() noexcept { return noise; }

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    int getLatencySamples() const override { return latency; }
    double getTailSeconds() const override;
    bool sleepsOnSilence() const noexcept override { return false; } // its noise must start with the song, sound or none

    static constexpr double pitchReadingsPerSecond = 100.0;
    static constexpr double holdSeconds = 2.0; // the noise plays on for this long after the sound coming in stops

private:
    // A channel's way through the record: the twice-the-rate stretch with the delay line, the highs the curve bends and the
    // curve, what keeps the sound in step with it, and the player's band.
    struct Channel
    {
        dsp::Oversampler2x oversampler;
        dsp::Filter highs;                    // what the curve is given, at twice the rate
        dsp::DelayLine line;                  // at twice the rate
        dsp::AntialiasedCurve bent, straight; // the curve, and a straight line played the same way
        std::array<float, 8> late {};         // the sound itself, as late as the curve
        int lateAt = 0;
        float residueLast = 0.0f, residueOut = 0.0f; // what the curve adds, without its lowest
        dsp::Filter lowCut, highCut;
    };

    double sampleRate = 44100.0;
    int latency = 0;
    double centre = 0.0;                // the delay line's middle, in samples at twice the rate
    double flutterReach = 0.0;          // the most the flutter moves it either way, in the same
    dsp::FilterCoefficients highsShape; // the curve's high-pass, at twice the rate
    float residuePole = 0.0f;
    float wowSmoothing = 0.001f, smoothing = 0.001f, bandSmoothing = 0.01f, splitSmoothing = 0.01f;
    std::array<Channel, 2> channels;

    // The wow: its depth and the record's speed, gliding, and how far it swings the delay (in samples at twice the rate), which
    // moves no faster than swingStep a sample, so that a deep wow turned up or a slow one sped up never swoops the pitch; where
    // the record is in its turn, as a turning pair (cos, sin); and the delay it set at the last sample.
    float wowNow = 0.0f, rateNow = 0.5f;
    double swingNow = 0.0, swingStep = 0.01;
    double turnCos = 1.0, turnSin = 0.0, stepCos = 1.0, stepSin = 0.0, stepRate = -1.0;
    double lastDelay = 0.0;
    int sinceNormalised = 0;

    // The flutter: noise through a Butterworth low-pass at its corner, made a delay, and its depth gliding.
    dsp::Random flutterNoise;
    dsp::Filter flutterFilter;
    dsp::FilterCoefficients flutterShape;
    float flutterNow = 0.0f, flutterScale = 1.0f, flutterSmoothing = 0.001f;

    float distortionNow = 0.0f, widthNow = 1.0f, bassAmount = 0.0f;
    double splitFrequency = 120.0;
    int sinceSplitMoved = 0;
    dsp::Crossover sideSplit, middlePhase;
    dsp::FilterCoefficients lowCutTarget, highCutTarget, lowCutNow, highCutNow;
    bool curveRests = true; // its curves have no sound in them

    SurfaceNoise noise;
    bool wasPlaying = false, heardSound = false;
    int silentFor = 0, holdSamples = 1;
    int restingFor = 0, restAfter = 1; // the samples nothing has come or played, and how many empty the delay line
    bool isFirstBlock = true;

    LiveValue turn { 0.0f }, pitchNow { 0.0f };
    SampleTap pitchTap { 2048 };
    int readingEvery = 441, sinceReading = 0;

    void restCurves() noexcept;
    void rest (int numSamples) noexcept;
    void turnBy (double samples) noexcept;
    float getTurnNow() const noexcept; // 0 to 1
    void setSplit (double frequency) noexcept;
};
} // namespace phazite::devices
