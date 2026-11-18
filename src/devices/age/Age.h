// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's Age: what a physical medium does to a sound, a record or a tape, as producers of lo-fi, hip-hop and synthwave reach
// for one to do (iZotope's Vinyl, XLN's RC-20, Ableton's Vinyl Distortion), each of its parts with a switch. Its Medium picks
// the record or the tape; a medium ignores the parameters that are the other's alone (the record's Crackle and Density, the
// tape's Dropouts and Head Bump). In the order a medium makes it:
//
// - The cut (the Stereo part's): the lows below Bass Mono summed to mono, as a cutting engineer sums them so that the stylus
//   stays in the groove (at its lowest, 20 Hz, off). The side's lows are taken off by a Linkwitz-Riley split, the middle kept
//   in phase with it.
// - Wobble: Wow is how far the pitch swings, in cents, and Wow Rate how fast. A record's wow is its warp, once a turn (0.56 Hz
//   is 33 1/3 rpm, 0.75 Hz is 45 and 1.3 Hz is 78): a warp swings in more than one hump, so a quarter as much again comes at
//   twice the rate, the record flat for longer than sharp (-1.25 to +0.75 times the depth). A tape's is its transport's slow,
//   uneven drift: 0.63 of a sine at the rate and 0.27 of noise low-passed at half the rate (as loud as a sine of that
//   depth), within the Wow but for a moment now and then, held inside the record's reach. Flutter is the drive's quicker, unsteady wobble: noise below 6 Hz on a record, between
//   4 and 14 Hz on a tape (a capstan's and its rollers'), its depth in cents RMS (times the square root of two, as a sine's).
//   Both move the sound through a delay line read at twice the rate by Lagrange interpolation (DelayLine.h), so the top keeps
//   its level as the delay moves.
// - Drive: on a record, the groove's distortion: as a stylus's round tip traces a groove that a sharp chisel cut, the loud
//   highs bend, more the louder and higher they are, and the bass hardly at all. Distortion drives what is above 2 kHz (a
//   high-pass of 12 dB an octave) into a lopsided curve, a second harmonic first and a third as it is pushed, up to 12 dB into
//   it. On a tape, the whole sound saturates, the highs first, as a tape is recorded with its top lifted: a high shelf of 6 dB
//   at 4 kHz, then a symmetric soft curve (odd harmonics), up to 9 dB into it (the highs 15) with as much taken back off,
//   then the shelf turned down again; and Head Bump, a bell at 60 Hz (Q 1.4) of as many decibels, the lift a playback head gives the lows.
//   Either way only what the curve bends is added to the sound, which itself passes untouched: at 0 % there is no curve at
//   all, and a quiet sound keeps its level. The curves play at twice the rate by their antiderivatives (dsp/Saturation.h),
//   what they add kept apart from the sound, taken through a high-pass at 20 Hz (so that a lopsided wave leaves no DC) and
//   added back in step with it.
// - Noise: on a record, its surface (SurfaceNoise.h): Hiss, the groove's hush, its RMS; Crackle, the largest clicks' peak, and
//   Density, how many come a second. On a tape (TapeWear.h): Hiss, the tape's own, brighter and each side its own; and
//   Dropouts, the moments the tape loses the head, the level dipping and the top going first, on the sound after the drive
//   and before the hiss. At their lowest, off. The noise plays while the song plays, from wherever it starts, and fades out
//   over 20 ms when it stops, and 2 s after the sound coming in has stopped, as a run-out groove or a tape's leader ends, so
//   that an export ends with the song rather than with ten seconds of noise. The crackle and the dropouts are where the song
//   is, the same however it is reached.
// - The player's Stereo: Width, how far apart its two sides are (100 % as they come, 0 % mono), with the cut above.
// - The Band: Low Cut and High Cut, Butterworth filters of 12 dB an octave that roll the medium's ends off, the noise's too.
//
// Each part (Wobble, Noise, Drive, Stereo, Band) has a switch. A part that is off does none of its work and passes the sound
// as the part before it left it; its values stay. A switch fades its part over 20 ms: the wobble's depth glides to nothing as
// it does when Wow is turned down, so that the pitch never jumps; the noise fades; the drive's amount glides to nothing; the
// width glides to 100 % and Bass Mono goes as it goes at its lowest; the band's filters fade in and out (as the Exciter's
// High Cut). A change of medium crossfades each part from one medium's way to the other's: the drive and the noise over
// 20 ms, the flutter over a quarter of a second, and the wow's depth glides to nothing in the old medium's shape and back up
// in the new one's.
//
// Every move glides: the wow's depth and rate over 50 ms, and how far they swing the delay no faster than would bend the
// pitch 7 cents, so that a deep wow turned up never swoops, and the rate rises no faster than that swing comes down, so
// that a slow, deep wow sped up bends the pitch no more than twice the Wow; the flutter's depth over 300 ms, the band's edges over 5 ms, the
// rest over 20 ms. The delay line's middle is the device's latency, the same in both media and with any part off: as deep a
// wow as the slowest rate allows, and the flutter's deepest, with the oversampling's few samples; about 11 ms. Its runner
// applies the mix, with what came in as late, which Age passes the sound's linear way with it (its oversampling, its delay
// and, while it is on, Bass Mono's all-pass), but not what moves and colours it, so that the two meet in phase at any mix: at
// no mix, what came in with its phase turned, its level within a few tenths of a decibel to 18 kHz.
//
// Its editor reads the turn (the record's, or the tape's reels'), the pitch now and its history, the clicks as they come and
// the dropouts. Real-time safe: nothing is allocated after prepare(), and while the song is stopped and nothing comes in it
// rests, after its delay line has emptied, until sound or the song comes again; its runner puts it to sleep only while the
// song is stopped, and wakes it as the song plays (playsWithTheSong()), so that its noise starts at a silent song's start.
#pragma once

#include "SurfaceNoise.h"
#include "TapeWear.h"
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
class Age final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteAge";

    enum Parameter
    {
        medium,     // the record or the tape (Medium)
        wobble,     // the Wobble part's switch
        wow,        // cents
        wowRate,    // hertz: the record's turns a second, or the tape's drift's rate
        flutter,    // cents RMS, times the square root of two
        noise,      // the Noise part's switch
        hiss,       // decibels RMS; at its lowest, off
        crackle,    // decibels, the largest clicks' peak; at its lowest, off (the record's)
        density,    // clicks a second, on average (the record's)
        dropouts,   // from 0 (none) to 1 (the tape's)
        drive,      // the Drive part's switch
        distortion, // from 0 (none) to 1
        headBump,   // decibels at 60 Hz (the tape's)
        stereo,     // the Stereo part's switch
        width,      // the two sides apart, from 0 (mono) to 1 (as they come)
        bassMono,   // below it, the lows in mono; at its lowest, off
        band,       // the Band part's switch
        lowCut,     // the band, from
        highCut,    // and to
        mix         // applied by its runner (DeviceInfo.h, mixParameterID)
    };

    enum class Medium
    {
        record,
        tape
    };

    static const DeviceInfo info;

    // The medium the values pick.
    static Medium getMedium (std::span<const float> parameters) noexcept;

    // Whether a part's switch is on, by its parameter (wobble, noise, drive, stereo or band).
    static bool isOn (std::span<const float> parameters, Parameter) noexcept;

    // Whether a level parameter (Hiss, Crackle) or Bass Mono is at its lowest, which turns it off.
    static bool isOff (std::span<const float> parameters, Parameter);

    // The pitch a record's wow plays a part of the way through a turn (0 to 1), in cents: for its editor, and tests.
    static float getWowCents (float wowCents, float turn) noexcept;

    // What the band does to a frequency, the Low Cut's and the High Cut's gain together (not in decibels): for its editor.
    static double getBandGain (std::span<const float> parameters, double frequency, double sampleRate);

    // The groove's curve as it bends a sample of the highs, at a distortion (0 to 1): the sample itself at 0. For tests and for
    // drawing it.
    static float shapeSample (float distortion, float sample) noexcept;

    // For its editor: how far through its turn the record (or the tape's reel) is (0 to 1); the pitch it plays now, in cents;
    // its pitch over the last moments, one reading every 10 ms (while a display listens); the record's noise, for the clicks
    // that came and the largest; and the tape's wear, for its dropouts.
    const LiveValue& getTurn() const noexcept { return turn; }
    const LiveValue& getPitch() const noexcept { return pitchNow; }
    SampleTap& getPitchTap() noexcept { return pitchTap; }
    SurfaceNoise& getSurfaceNoise() noexcept { return noiseOfRecord; }
    TapeWear& getTapeWear() noexcept { return wear; }

    // How many times its copy for the mix has been made again from what came in lately, as the Mix left 100 % after resting
    // there; and how many samples back it reads again for it, at any rate (the copy keeps its way into its delay line at 100 %). For
    // tests.
    int getDryRebuilds() const noexcept { return dryRebuilds; }
    int getDryRebuildSamples() const noexcept;

    // Whether it rested in the last block: nothing coming in, nothing playing or about to, its delay line empty. For tests.
    bool isResting() const noexcept { return rested; }

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    int getLatencySamples() const override { return latency; }
    bool delaysDry() const noexcept override { return true; }
    double getTailSeconds() const override;
    bool playsWithTheSong() const noexcept override { return true; } // its noise must start with the song, sound or none

    static constexpr double pitchReadingsPerSecond = 100.0;
    static constexpr double holdSeconds = 2.0; // the noise plays on for this long after the sound coming in stops

private:
    // A channel's way through the medium: the twice-the-rate stretch with the delay line, the curves and what they are given,
    // what keeps the sound in step with them, the head bump and the band.
    struct Channel
    {
        dsp::Oversampler2x oversampler;
        dsp::Filter highs;                    // what the record's curve is given, at twice the rate
        dsp::DelayLine line;                  // at twice the rate
        dsp::AntialiasedCurve bent, straight; // the record's curve, and a straight line played the same way
        std::array<float, 8> late {};         // the sound itself, as late as the curve
        int lateAt = 0;
        float residueLast = 0.0f, residueOut = 0.0f;  // what the record's curve adds, without its lowest
        float tapeLast = 0.0f, tapeOut = 0.0f;        // and the tape's: a symmetric curve leaves DC of a lopsided wave too
        dsp::Filter lifted, lowered;                  // the tape's shelf up, before its curve, and down, after
        dsp::AntialiasedCurve tapeBent, tapeStraight; // the tape's curve, and a straight line
        dsp::Filter bump;                             // the head bump's band
        dsp::Filter lowCut, highCut;
    };

    // A channel of the copy of what came in for its mix (ProcessContext::dry), which it delays itself: at every mix, the
    // sound's linear way, through Bass Mono's all-pass (while it is on) and twice the rate, through a delay line read at its
    // middle, as late as the curve. Mixed with a copy merely delayed, the sound cancelled itself where those turn its phase:
    // half and half, the top octave (as the saturator's did) and the middle at Bass Mono's frequency.
    struct DryWay
    {
        dsp::Crossover phase;
        dsp::Oversampler2x upper, lower; // to twice the rate, and back (each used one way)
        dsp::DelayLine line;             // at twice the rate
        dsp::DelayLine plain;            // what came in, merely delayed

        // A sample through Bass Mono's all-pass into the delay line at twice the rate, at every mix (cheap: the copy is
        // made again from it); and on from there the rest of the sound's way, read as far back as it says.
        void feed (float phasedSample) noexcept;
        float turn (float phasedSample, double back) noexcept;

        // The way back to the sound's rate made again, from silence, over the delay line's reads of the last samples as
        // they were; and the whole way silent.
        void rebuild (int samples, double back) noexcept;
        void restTurn() noexcept;
    };

    double sampleRate = 44100.0;
    int latency = 0;
    std::array<DryWay, 2> dryWays;
    float lastMix = 1.0f;            // the Mix in the last block
    bool dryRan = false;             // the copy went the sound's way in the last block
    int sinceMixed = 0, dryHold = 1; // samples since the Mix was last under 100 % (up to the hold), and the hold
    int dryRebuilds = 0;
    double centre = 0.0;                           // the delay line's middle, in samples at twice the rate
    double wowReach = 0.0;                         // the most the wow moves it either way, in the same
    double flutterReach = 0.0;                     // the most the flutter moves it either way, in the same
    dsp::FilterCoefficients highsShape;            // the record's curve's high-pass, at twice the rate
    dsp::FilterCoefficients liftShape, lowerShape; // the tape's shelves, at twice the rate
    dsp::FilterCoefficients bumpShape;             // the head bump's band
    float residuePole = 0.0f, tapePole = 0.0f;
    float wowSmoothing = 0.001f, smoothing = 0.001f, bandSmoothing = 0.01f, splitSmoothing = 0.01f;
    float fadeStep = 0.001f, flutterFadeStep = 0.0001f; // a share's step a sample over 20 ms, and the flutter's over 250 ms
    std::array<Channel, 2> channels;

    // The medium: how far the drive and the noise are the tape's (0 to 1), and the flutter; and the wow's shape, which turns
    // only while its depth is nothing.
    float tapeShare = 0.0f, flutterTapeShare = 0.0f;
    Medium wowShape = Medium::record;

    // The wow: its depth and its rate, gliding, and how far it swings the delay (in samples at twice the rate), which
    // moves no faster than swingStep a sample, so that a deep wow turned up or a slow one sped up never swoops the pitch; where
    // the record is in its turn, as a turning pair (cos, sin); and the delay it set at the last sample.
    float wowNow = 0.0f, rateNow = 0.5f;
    double swingNow = 0.0, swingStep = 0.01;
    double turnCos = 1.0, turnSin = 0.0, stepCos = 1.0, stepSin = 0.0, stepRate = -1.0;
    double lastDelay = 0.0;
    int sinceNormalised = 0;

    // The tape's drift: noise through a Butterworth low-pass at half the rate, made a delay, at the rate it was made for,
    // and what makes its RMS 1.
    dsp::Random driftNoise;
    dsp::Filter driftFilter;
    dsp::FilterCoefficients driftShape;
    double driftRate = -1.0;
    float driftScale = 1.0f;

    // The flutter: the record's noise through a Butterworth low-pass at its corner, made a delay; the tape's, its pitch noise
    // in a band, gathered into a delay; and its depth gliding.
    dsp::Random flutterNoise, tapeFlutterNoise;
    dsp::Filter flutterFilter, tapeFlutterLow, tapeFlutterHigh;
    dsp::FilterCoefficients flutterShape, tapeFlutterLowShape, tapeFlutterHighShape;
    double tapeFlutterDelay = 0.0, tapeFlutterLeak = 1.0; // a flutter of a whole unit's depth, which its depth then scales
    float flutterNow = 0.0f, flutterScale = 1.0f, tapeFlutterScale = 1.0f, flutterSmoothing = 0.001f;

    float distortionNow = 0.0f, widthNow = 1.0f, bassAmount = 0.0f, bumpNow = 0.0f, bandShare = 1.0f;
    double splitFrequency = 120.0;
    int sinceSplitMoved = 0;
    dsp::Crossover sideSplit, middlePhase;
    dsp::FilterCoefficients lowCutTarget, highCutTarget, lowCutNow, highCutNow;
    bool curveRests = true, tapeCurveRests = true; // its curves have no sound in them

    SurfaceNoise noiseOfRecord;
    TapeWear wear;
    bool wasPlaying = false, heardSound = false;
    int silentFor = 0, holdSamples = 1;
    int restingFor = 0, restAfter = 1; // the samples nothing has come or played, and how many empty the delay line
    bool isFirstBlock = true;
    bool rested = false;

    LiveValue turn { 0.0f }, pitchNow { 0.0f };
    SampleTap pitchTap { 2048 };
    int readingEvery = 441, sinceReading = 0;

    void restCurves() noexcept;
    void restTapeCurves() noexcept;
    void restDrift() noexcept;
    void restTapeFlutter() noexcept;
    void restBand() noexcept;
    void restDry() noexcept;
    void rest (int numSamples) noexcept;
    void turnBy (double samples) noexcept;
    float getTurnNow() const noexcept; // 0 to 1
    void setSplit (double frequency) noexcept;
};
} // namespace phazite::devices
