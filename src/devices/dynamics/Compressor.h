// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's compressor (roadmap steps 1.6 and S6.3), in one of three modes, as Fruity Limiter's tabs switch its own (the
// owner, 2026-11-11: "should the line graph even co-exist in the same effect as sidechaining"; docs/design.md, section 9):
// Compress, Duck and Transient, one device doing what other DAWs ship as a compressor, a gate and a transient shaper (the
// owner's rule, 2026-11-17: one generic device that does its whole job).
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
// Clean's and VCA's soft clipping), its curve faded in from a straight line over the first tenth of its travel, so that the
// sound never jumps as it leaves 0 (nor as FET's or Tube's colour begins to drive it); at twice the sample rate, its curve
// played by its antiderivative (dsp/Oversampling.h), so that what it adds doesn't fold back as aliasing and what it leaves
// straight stays level to 20 kHz (at the sound's own rate the antiderivative alone averaged every two samples: 5 dB down at
// 15 kHz, of the whole sound, whenever the drive or FET's or Tube's colour engaged); undriven, the sound takes a plain delay
// as long instead, which the drive's comes exactly as late as (dsp/AlignedShaper.h). It hears its own sound, or, while its
// sidechain listens to a track, that track (a keyed compressor, as the compressors professionals own take a key), through the
// key's low cut, so that bass notes don't pump it, as a bus compressor's sidechain high-pass does, and its high cut. Its
// Look-Ahead delays the sound while it hears it as it comes, so that the gain is down as a peak plays; its Hold has it hear
// the loudest it heard over so many milliseconds (as Duck hears its key), so that a gate drawn on its curve opens once and
// closes once on a ringing drum; and its Auto Makeup adds half what its curve takes off a 0 dB level.
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
// Transient: it turns each hit's attack and what follows it up or down by its punch and its sustain, whatever the sound's
// level, as the differential-envelope transient shapers do (the SPL Transient Designer's principle): four followers of its
// own sound's level in decibels, a fast and a slow one rising, a long and a short one falling, the attack how far the fast
// is over the slow, what follows it how far the long is over the short; on a steady sound each pair agrees, and nothing
// turns.
//
// In each, it turns both channels alike, so that the stereo image holds. Its meters show how loud the detector hears (its
// own sound, or the key) and how far it turns the sound down and up; while its editor asks, it plays what its detector hears
// itself, as its cuts leave it, to hear what it listens to.
#pragma once

#include "DynamicsCurve.h"
#include "devices/core/Clock.h"
#include "devices/core/DeviceInfo.h"
#include "devices/core/MeterValue.h"
#include "devices/core/Processor.h"
#include "devices/dsp/AlignedShaper.h"
#include "devices/dsp/DelayLine.h"
#include "devices/dsp/Envelopes.h"
#include "devices/dsp/Filters.h"
#include "devices/dsp/Oversampling.h"
#include "devices/dsp/Saturation.h"

#include <juce_core/juce_core.h>

#include <algorithm>
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
        lookahead, // Compress's, in milliseconds: how far its detector hears ahead of the sound
        threshold, // Duck's, in decibels: the key's level that starts it
        depth,     // Duck's: how far it turns the sound down, in decibels
        attack,
        hold,      // Duck's: the least it stays down from a hit, in milliseconds
        levelHold, // Compress's, in milliseconds: how long its detector holds the loudest level it heard
        release,
        releaseCurve, // Duck's: from -1 (quick at first) through 0 (straight in decibels) to 1 (slow at first)
        makeup,       // Compress's and Transient's
        autoMakeup,   // Compress's: on (1) or off (0)
        drive,        // Compress's: from 0 (none) to 1
        punch,        // Transient's: from -1 (each hit's attack turned down) to 1 (up)
        sustain,      // Transient's: from -1 (what follows the attack turned down) to 1 (up)
        speed,        // Transient's, in milliseconds: how long an attack is taken to last
        keyLowCut,
        keyHighCut,
        points,                               // Compress's curve (DynamicsCurve.h): how many points it has, then its points
        mix = points + curves::parameterCount // applied by its runner (DeviceInfo.h, mixParameterID)
    };

    enum class Mode
    {
        compress, // its own sound or its key, by its curve
        duck,     // under its key, by its threshold, depth, attack, hold and release
        transient // each hit's attack and what follows it, by its punch, sustain and speed
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
    static Mode getMode (float value) noexcept { return value >= 1.5f ? Mode::transient : value >= 0.5f ? Mode::duck : Mode::compress; }

    // How long the detector holds the key's loudest level while it ducks, in milliseconds: at least half a cycle of a kick's
    // lowest notes (a 50 Hz kick's is 10 ms), so that the level it hears doesn't fall to nothing as the key's wave crosses 0,
    // which would let the sound go between the cycles of one kick. Transient's ear holds its level as long, for the same
    // reason: so that a steady sound reads as steady.
    static constexpr int keyPeakMilliseconds = 10;

    // The longest Compress's detector holds the loudest level it heard (its Hold), in milliseconds, and the longest it looks
    // ahead (its Look-Ahead).
    static constexpr int mostLevelHoldMilliseconds = 500;
    static constexpr float mostLookaheadMilliseconds = 10.0f;

    // How many decibels its automatic makeup adds for a curve: half what the curve takes off a 0 dB input (below 0, for a
    // curve that adds level there).
    static float getAutoMakeupDecibels (const curves::Gains& gains) noexcept { return -0.5f * gains.getDecibels (1.0f); }

    // The loudest the detector heard, in decibels, and the most the compressor turned the sound down by and up by, in
    // decibels (0 or more), since its editor last looked (devices/core/MeterValue.h).
    MeterValue& getLevelMeter() noexcept { return levelMeter; }
    MeterValue& getReductionMeter() noexcept { return reductionMeter; }
    MeterValue& getBoostMeter() noexcept { return boostMeter; }

    // Message thread: plays the key, as the detector hears it, instead of the sound, for the next half second by the clock;
    // its editor asks again each time it refreshes, so that the key stops when the editor no longer shows, whether the
    // device played meanwhile or not. It fades between the two over a few milliseconds, and its runner plays it whole,
    // whatever the mix.
    void listenToKey() noexcept { listenUntil.store (Clock::getMilliseconds() + listenMilliseconds, std::memory_order_relaxed); }

    bool isPlayingKey() const noexcept { return Clock::getMilliseconds() < listenUntil.load (std::memory_order_relaxed); }
    bool overridesMix() const noexcept override { return keyShare > 0.0f; } // the block it last played: the key, or fading
    const FallingValue* getGainReduction() const noexcept override { return &gainReduction; }

    // Ducking, it looks ahead: its sound comes this much later than it goes in, while it hears its key as it comes, so that
    // it is down already as the hit plays. Without, a note that starts with the kick played its first milliseconds at full
    // level before the attack caught it, and then went down: a click under each kick (the owner heard it in Exodus,
    // 2026-11-12), which the kick covers in the mix but not alone. Its attack ends at the hit while it is no longer than this,
    // its hold still counting from the hit; a longer one starts this much before it. Compressing, it looks ahead by its
    // Look-Ahead, and its sound comes as late as that and as its drive plays it, a few samples (getDriveLatency()), driven or
    // not; shaping transients, as late as its drive alone. The engine makes up for the latency on the other tracks, and Low
    // Latency leaves a compressor whose latency is over a millisecond out while it is on
    // (model/playback/internal/PlaybackHost.h), as it does a limiter's.
    static constexpr double duckLookaheadSeconds = 0.010;

    // How late the drive plays the sound, in samples, at any rate: its filters to twice the rate and back, and its curve.
    static int getDriveLatency();

    int getLatencySamples() const override { return latency; }
    bool delaysDry() const noexcept override { return true; }

    // How long its gain goes on moving after what comes in has stopped, in seconds, so that its runner lets it finish before
    // it lets it sleep: asleep, its gain would stand still, and the next phrase would come turned as it was as it slept, live
    // only (a render never sleeps). Compressing, its Hold and the time its slower way (Attack or Release, Opto's slow
    // stage) takes to come within a tenth of a decibel across as far as its curve can move its gain; ducking, its Hold and
    // Release; shaping transients, its long follower's fall; at most maxTailSeconds. Worked out as it plays a block, read by
    // any thread.
    double getTailSeconds() const override { return tail.load (std::memory_order_relaxed); }
    static constexpr float maxTailSeconds = 10.0f;
    int getLatencySamplesFor (std::span<const float> valuesSetByHand) const override;
    void takeValuesBeforePrepare (std::span<const float> valuesSetByHand) override;

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;

private:
    // The loudest of a level over its last few milliseconds, a millisecond at a time: each finished millisecond's loudest
    // (the last mostMilliseconds of them kept), and the one going on now's. Duck's key, Compress's Hold and Transient's ear.
    template <int mostMilliseconds>
    struct RunningPeak
    {
        std::array<float, (size_t) mostMilliseconds> peaks {};
        int index = 0, samples = 0, samplesPerPeak = 48;
        float now = 0.0f, before = 0.0f; // the millisecond going on, and the loudest of those before it

        void reset() noexcept
        {
            peaks = {};
            index = 0;
            samples = 0;
            now = 0.0f;
            before = 0.0f;
        }

        // The loudest over the millisecond going on and the given number of those before it (1 to mostMilliseconds).
        float process (float level, int milliseconds) noexcept
        {
            now = std::max (now, level);

            if (++samples >= samplesPerPeak)
            {
                peaks[(size_t) index] = now;
                index = (index + 1) % mostMilliseconds;
                samples = 0;
                now = 0.0f;
                before = 0.0f;

                for (int back = 1; back <= milliseconds; ++back)
                    before = std::max (before, peaks[(size_t) ((index - back + mostMilliseconds) % mostMilliseconds)]);
            }

            return std::max (before, now);
        }
    };

    void compress (const ProcessContext&);
    void duck (const ProcessContext&);

    // From the mode it last played in to another: the gain goes on from where it was, and what is on its way is turned by the
    // mode it was meant for (below, curveGainDelay).
    void handOver (Mode to, std::span<const float> values) noexcept;
    void shapeTransients (const ProcessContext&);

    // Transient's ear: the louder channel through the key's low cut, its loudest over keyPeakMilliseconds (hearLevel(), which
    // runs in every mode, two filters and a comparison, so that it has the level as it switches to Transient); and in Transient
    // alone, that in decibels and its four followers of it, as its punch and sustain take them (shapeTransients()). Switching
    // to Transient, its followers start at the level heard then, so that no burst of gain comes as it starts.
    float hearLevel (float left, float right, const dsp::FilterCoefficients& lowCut) noexcept;
    void hearTransients (float left, float right, const dsp::FilterCoefficients& lowCut) noexcept;
    void startFollowers() noexcept;

    // Sets the glides of its gains and its drive where they are set, at the first block it plays from a reset.
    void startGlides (std::span<const float> values, float targetMakeup) noexcept;

    double sampleRate = 44100.0;
    float makeupSmoothing = 0.001f;                                        // of the way to the makeup gain, per sample
    float makeupDecibels = 0.0f, inputDecibels = 0.0f, driveAmount = 0.0f; // the drive's, the knob's alone
    float lastInputDecibels = 0.0f, lastInput = 1.0f;                      // the input gain last worked out
    float lastGainDecibels = 0.0f, lastGain = 1.0f;                        // the gain last worked out
    float lastLevel = -1.0f, lastWanted = 0.0f;                            // the level last heard, and the gain its curve wanted
    float colourAmount = 0.0f;                                             // how far it turns down, followed smoothly, for its colour
    float lastDriveAmount = -1.0f, driveGain = 1.0f;                       // into drive's curve
    float lastMakeupDecibels = 0.0f, makeupGain = 1.0f;                    // after it
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

    // Drive: each channel's sound at twice the rate, through the curve and through a straight line, each played by its
    // antiderivative (first-order antiderivative anti-aliasing: Parker, Zavalishin and Le Bivic, 2016; dsp/Saturation.h), and
    // what the curve adds to the straight line, the DC that the even harmonics leave filtered out of it, added as far as the
    // drive has faded the curve in (curveShare); back at the sound's rate, then a part of a sample more, so that it comes
    // exactly as late as the plain delay at low frequencies (dsp/AlignedShaper.h). The sound goes through it while Drive is up
    // or the character has a colour of its own (FET, Tube), the straight line alone while nothing colours it; otherwise through
    // a plain delay as long, the two faded between over 1.5 ms.
    struct Drive
    {
        dsp::Oversampler2x oversampler;
        dsp::AntialiasedCurve bent, straight;
        dsp::Filter dcBlocker; // on what the curve adds, at twice the rate
        dsp::FractionalDelay rest;
        bool curveRested = false; // the curve played no part in the last sample

        void reset() noexcept;

        template <typename Curve>
        float play (float sample, float gain, const Curve&, float share, const dsp::FilterCoefficients& dcBlocker, bool blocksDc) noexcept;
    };

    std::array<Drive, 2> drives, nextDrives; // the curve playing, and another warming beside it to take over
    std::array<dsp::DelayLine, 2> undriven;
    float driveBlend = 0.0f;      // how far the sound goes through the drive
    float driveBlendStep = 0.01f; // its step a sample as it fades
    float curveShare = 0.0f;      // how far the drive has faded its curve in from a straight line, from 0 to 1
    float knobShare = 0.0f;       // the knob's part of it
    bool drivesRan = false;       // whether the drive played in the last block
    int driveWarm = 0;            // the samples it has played since it started, from silence
    int warmUp = 1;               // how many it plays first, from silence, before it is faded in
    int evenWarmUp = 1;           // how many it plays first for a curve with an offset to take out
    bool isFresh = true;          // nothing played since it was reset: silence in all of it
    float playingEven = 0.0f;     // the curve's share of even harmonics, as it plays
    bool changesCurve = false;    // another curve warming in nextDrives, then faded to
    float nextEven = 0.0f;        // its share of even harmonics
    int nextWarm = 0, nextWarmUp = 1;
    float nextBlend = 0.0f; // how far the sound has faded from the curve playing to it
    int driveLatency = 0;

    // How many samples the drive plays from silence before it is heard, for a curve's even share and how far it is in.
    int getDriveWarmUp (float even, float share) const noexcept;

    // The copy of what came in for its mix (ProcessContext::dry): as late as the sound, the same way through (the drive's
    // plain delay, then a delay for the rest of the latency where that is Duck's), and while the sound goes through the drive,
    // through the drive's filters and straight line with it, faded alike: the filters to twice the rate and back turn the phase
    // of the top octave, and the copy is turned as the sound is, so that the two meet in phase at any mix.
    std::array<dsp::AlignedShaper, 2> dryDrives;
    std::array<dsp::DelayLine, 2> dryDelays, dryRests;

    // Duck's: the key's loudest over the last keyPeakMilliseconds; how far it turns the sound down now, in decibels; the
    // samples since the last hit, the key rising over the threshold (counted up to the most a hold can be); and the release
    // going on: where it started, and how far along it is (0 to 1).
    RunningPeak<keyPeakMilliseconds> keyPeak;
    float lastKeyPeak = -1.0f, keyDecibels = -120.0f;
    float ducked = 0.0f, lastDucked = -1.0f, duckGain = 1.0f;
    int sinceHit = 0, sinceUnder = 0; // the samples since the last hit, and since the key went under its threshold
    float releaseFrom = 0.0f;
    double releaseAlong = 0.0;
    bool releasing = false, wasOver = false;

    // Compress's Hold: the level it heard, its loudest over the last Hold milliseconds.
    RunningPeak<mostLevelHoldMilliseconds> levelPeak;

    // Transient's: its ear (hearTransients()), the level in decibels and its four followers, which rise or fall at different
    // times (fast against slow for the attack, long against short for what follows it); and its gain in decibels, glided.
    std::array<dsp::Filter, 2> transientCuts {};
    RunningPeak<keyPeakMilliseconds> transientPeak;
    dsp::LevelFollower fastFollower, slowFollower, longFollower, shortFollower;
    float transientGlide = 0.01f;           // of the way to the gain wanted, per sample
    float transientDecibels = 0.0f;         // the gain it turns the sound by, without the makeup
    float transientLevel = -90.0f;          // in decibels, as its followers last heard it
    float transientHeld = 0.0f;             // its loudest over the last keyPeakMilliseconds, which it hears in every mode
    std::array<float, 2> transientHeard {}; // each channel, through the low cut, as it last heard it
    Mode lastMode = Mode::compress;         // the mode it last played in, for the hand-over to another
    bool playedYet = false;                 // since it was reset

    // Its latency, in samples: Duck's look-ahead (duckLookaheadSeconds) while it was given Duck before it was prepared, the
    // drive's and the Look-Ahead's while it was given Compress, the drive's while it was given Transient. What it delays by
    // it: the sound and the key it plays, each channel's; Duck's gain, by the look-ahead less the attack (so that a short
    // attack ends at the hit; ducking without having been prepared for it, a mode that automation changed, it doesn't look
    // ahead); and Compress's and Transient's gain, by what is left of the latency over the drive's less the Look-Ahead, the
    // place it is read from gliding an eighth of a sample a sample as automation moves the Look-Ahead, so that the gain
    // never steps. In every mode the sound takes the same way through: a delay for the rest of the latency over the drive's,
    // then the drive's delay (or the drive). Each sample is owned by the mode that played as it came in, which a ring as long
    // as the delays keeps (owners): Compress or Transient turn what they own as it leaves the first delay, Duck as it leaves
    // the drive's, each by its own gain worked out for that sample, or, where it had stopped playing before it could look
    // ahead at it, the one it had as it stopped (each mode pushes the other's gain held, as it was). Around the first sample
    // of another owner, the two gains are faded between over 5 ms, in decibels (shares), so that no change of mode steps the
    // gain. So automation that changes the mode, however often, plays on what is on its way, nothing lost, played again,
    // turned by both modes or by neither.
    Mode preparedMode = Mode::compress;
    float preparedLookahead = 0.0f; // in milliseconds
    int latency = 0;
    bool prepared = false;
    std::array<dsp::DelayLine, 2> soundDelays, keyDelays;
    dsp::DelayLine gainDelay;      // Duck's gain
    dsp::DelayLine curveGainDelay; // Compress's and Transient's, its makeup with it
    dsp::DelayLine reductionDelay; // Compress's reduction, for FET's and Tube's colour, as late as its gain
    dsp::DelayLine makeupDelay;    // Compress's makeup alone, for its drive, as late as its gain
    dsp::DelayLine owners;         // each sample's owner as it came in: 1 Compress or Transient, 0 Duck
    dsp::DelayLine shares;         // how much of each sample Compress's or Transient's gain turns, as it left the first delay
    float curveLate = -1.0f;       // where Compress's and Transient's gain is read from, in samples (none yet below 0)
    float curveTarget = -1.0f;     // where it glides to (none below 0: it stays)
    int duckLate = 0;              // where Duck's gain is read from, in samples
    float ownedShare = -1.0f;      // the share of the sample leaving the first delay, as it moves to its owner's
    float shareStep = 0.004f;      // its step a sample: over 5 ms
    dsp::DelayLine curvesTurned;   // what Compress's or Transient's gain turned each sample by, as it left the first delay
    int changeLeft = 0;            // samples until a change of mode is through, as far as what goes out goes
    bool changing = false;         // the gain going out still held to changeSlew, catching up
    float changedDecibels = 0.0f;  // what the gain going out was last, in decibels
    float lastTurned = 1.0f;       // and what it was, as no change was going on

    // The gains for the sample at each place on its way, from each mode's gains for now (the one not playing pushing its
    // own held), the sample coming in owned by the mode playing (curveOwns): Compress's or Transient's as it leaves the first
    // delay (curve), with the makeup the drive takes off and puts back and the reduction FET's and Tube's colour follow, and
    // Duck's as it leaves the drive's (duck), each 1 for a sample the other mode owns, and faded between them about a change.
    struct Turned
    {
        float curve = 1.0f, makeup = 1.0f, amount = 0.0f, duck = 1.0f;
    };

    Turned route (bool curveOwns, float curveGainNow, float makeupNow, float amountNow, float duckGainNow) noexcept;
    void startRoute (bool curveOwns) noexcept; // as the first mode plays: everything on its way owned by it
    float getShareAhead() const noexcept;      // the share of the next sample to come in, once it reaches its place

    // Fills the gains' delays with none (unity), as a reset leaves them.
    void clearGainDelays() noexcept;

    // The latency for a mode and a Look-Ahead (in milliseconds) set by hand.
    int getLatencyFor (Mode, float lookaheadMilliseconds) const noexcept;

    std::atomic<float> tail { 0.0f }; // getTailSeconds()'s
    void workOutTail (Mode, std::span<const float> values) noexcept;

    static_assert (std::atomic<double>::is_always_lock_free);
};
} // namespace phazite::devices
