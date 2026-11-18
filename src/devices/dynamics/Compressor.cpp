// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Compressor.h"
#include "devices/dsp/Math.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace phazite::devices
{
namespace
{
    constexpr std::string_view modes[] = { "Compress", "Duck", "Transient" };
    constexpr std::string_view characters[] = { "Clean", "VCA", "FET", "Opto", "Tube" };

    // A point of its curve. At first it compresses 4:1 over -18 dB (curves::makeCompression(), as a test checks).
#define PHAZITE_COMPRESSOR_POINT(n, x, y, t)                                                                                               \
    { .id = "point" #n "X", .name = "Point " #n " X", .defaultValue = x, .unit = Unit::percent },                                          \
        { .id = "point" #n "Y", .name = "Point " #n " Y", .defaultValue = y, .unit = Unit::percent },                                      \
        { .id = "point" #n "Tension",                                                                                                      \
          .name = "Point " #n " Tension",                                                                                                  \
          .minimum = -1.0f,                                                                                                                \
          .maximum = 1.0f,                                                                                                                 \
          .defaultValue = t,                                                                                                               \
          .unit = Unit::percent },                                                                                                         \
    {                                                                                                                                      \
        .id = "point" #n "Type", .name = "Point " #n " Type", .maximum = (float) std::size (curves::pointTypeNames) - 1.0f,                \
        .unit = Unit::choice, .choices = curves::pointTypeNames                                                                            \
    }

    // The IDs are saved in projects.
    constexpr ParameterInfo parameters[] = {
        { .id = "mode",
          .name = "Mode",
          .maximum = (float) std::size (modes) - 1.0f,
          .defaultValue = 0.0f,
          .unit = Unit::choice,
          .choices = modes },
        { .id = "character",
          .name = "Character",
          .maximum = (float) std::size (characters) - 1.0f,
          .defaultValue = 0.0f,
          .unit = Unit::choice,
          .choices = characters },
        { .id = "inputGain", .name = "Input Gain", .minimum = -24.0f, .maximum = 24.0f, .unit = Unit::decibels },
        { .id = "lookahead",
          .name = "Look-Ahead",
          .minimum = 0.0f,
          .maximum = Compressor::mostLookaheadMilliseconds,
          .defaultValue = 0.0f,
          .unit = Unit::milliseconds },
        { .id = "threshold", .name = "Threshold", .minimum = -60.0f, .maximum = 0.0f, .defaultValue = -24.0f, .unit = Unit::decibels },
        { .id = "depth",
          .name = "Depth",
          .minimum = 0.0f,
          .maximum = 60.0f,
          .defaultValue = 12.0f,
          .unit = Unit::decibels,
          .midpoint = 15.0f },
        { .id = "attack",
          .name = "Attack",
          .minimum = 0.1f,
          .maximum = 200.0f,
          .defaultValue = 10.0f,
          .unit = Unit::milliseconds,
          .midpoint = 10.0f },
        { .id = "hold",
          .name = "Hold",
          .minimum = 0.0f,
          .maximum = 1000.0f,
          .defaultValue = 0.0f,
          .unit = Unit::milliseconds,
          .midpoint = 100.0f },
        { .id = "levelHold",
          .name = "Level Hold",
          .minimum = 0.0f,
          .maximum = (float) Compressor::mostLevelHoldMilliseconds,
          .defaultValue = 0.0f,
          .unit = Unit::milliseconds,
          .midpoint = 60.0f },
        // From a tenth of a millisecond, as the Attack, so that a gate drawn on the curve opens at once (it opens at the Release);
        // its midpoint where it was, so that the settings from about 50 ms up sit where they sat on the knob's travel.
        { .id = "release",
          .name = "Release",
          .minimum = 0.1f,
          .maximum = 2000.0f,
          .defaultValue = 120.0f,
          .unit = Unit::milliseconds,
          .midpoint = 100.0f },
        { .id = "releaseCurve", .name = "Release Curve", .minimum = -1.0f, .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::percent },
        { .id = "makeup", .name = "Makeup", .minimum = -12.0f, .maximum = 24.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
        { .id = "autoMakeup", .name = "Auto Makeup", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::toggle },
        { .id = "drive", .name = "Drive", .defaultValue = 0.0f, .unit = Unit::percent },
        { .id = "punch", .name = "Punch", .minimum = -1.0f, .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::percent },
        { .id = "sustain", .name = "Sustain", .minimum = -1.0f, .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::percent },
        { .id = "speed",
          .name = "Speed",
          .minimum = 1.0f,
          .maximum = 50.0f,
          .defaultValue = 10.0f,
          .unit = Unit::milliseconds,
          .midpoint = 10.0f },
        { .id = "keyLowCut",
          .name = "Sidechain Low Cut",
          .minimum = 20.0f,
          .maximum = 500.0f,
          .defaultValue = 20.0f,
          .unit = Unit::hertz,
          .scale = Scale::logarithmic },
        { .id = "keyHighCut",
          .name = "Sidechain High Cut",
          .minimum = 100.0f,
          .maximum = 20000.0f,
          .defaultValue = 20000.0f,
          .unit = Unit::hertz,
          .scale = Scale::logarithmic },
        { .id = "points", .name = "Points", .minimum = 2.0f, .maximum = (float) curves::maxPoints, .defaultValue = 4.0f },
        PHAZITE_COMPRESSOR_POINT (1, 0.0f, 0.0f, 0.0f),
        PHAZITE_COMPRESSOR_POINT (2, 0.102206f, 0.102206f, 0.0f),
        PHAZITE_COMPRESSOR_POINT (3, 0.5f, 0.160455f, 0.100808f),
        PHAZITE_COMPRESSOR_POINT (4, 1.0f, 0.213277f, 0.00657f),
        PHAZITE_COMPRESSOR_POINT (5, 1.0f, 0.213277f, 0.00657f),
        PHAZITE_COMPRESSOR_POINT (6, 1.0f, 0.213277f, 0.00657f),
        PHAZITE_COMPRESSOR_POINT (7, 1.0f, 0.213277f, 0.00657f),
        PHAZITE_COMPRESSOR_POINT (8, 1.0f, 0.213277f, 0.00657f),
        { .id = mixParameterID, .name = "Mix", .defaultValue = 1.0f, .unit = Unit::percent },
    };

#undef PHAZITE_COMPRESSOR_POINT

    static_assert (std::size (parameters) == (size_t) Compressor::mix + 1);
    static_assert (std::size (modes) == (size_t) Compressor::Mode::transient + 1);
    static_assert (std::size (characters) == (size_t) Compressor::Character::tube + 1);

    constexpr double makeupSeconds = 0.02;   // how long a change of the input or makeup gain, or of the drive, takes, about
    constexpr float highCutOff = 19999.0f;   // the key's high cut at its top: off
    constexpr double keyFadeSeconds = 0.005; // between the key and the sound, as Listen turns on or off

    // The characters' ears and their timing.
    constexpr double rmsSeconds = 0.01;                                // the average a VCA, Opto or Tube hears, over about this long
    constexpr float opticalSlowdown = 6.0f;                            // after its first half, an Opto lets go this many times slower...
    constexpr float opticalMemorySlowdown = 12.0f;                     // ...and up to this many more times, after holding for a while
    constexpr double memoryRiseSeconds = 2.0, memoryFallSeconds = 6.0; // how long holding takes to count, and to be forgotten
    constexpr float memoryHold = 3.0f;                                 // decibels of reduction that count as holding

    // Drive: up to this many decibels into the curve, and how much of the even harmonics' curve each character adds to the
    // odd harmonics' (none for Clean and FET, a little for VCA, more for Opto and the most for Tube).
    constexpr float mostDrive = 18.0f;
    constexpr float evenShares[] = { 0.0f, 0.05f, 0.0f, 0.15f, 0.3f };
    constexpr double dcBlockerFrequency = 10.0; // under what the even harmonics leave

    // FET's and Tube's own colour: as much drive as this, added to the knob's, once they turn down by colourReduction
    // decibels, and as much less as they turn down less (an 1176's distortion grows with its gain reduction, and a variable-mu
    // tube's with how far its bias shifts).
    constexpr float colourShares[] = { 0.0f, 0.0f, 0.1f, 0.0f, 0.1f };
    constexpr float colourReduction = 12.0f;

    // Duck: how far over its threshold the key has to be for the whole depth (less, a share of it), and the most samples a
    // hit is counted from (a hold of a second at 384 kHz, and more).
    constexpr float duckKnee = 6.0f;
    constexpr int mostSinceHit = 1 << 30;

    // Transient, as the differential-envelope shapers work (the SPL Transient Designer's principle), all in decibels of the
    // level its ear hears, from transientFloor up: two followers that differ in how fast they rise (in transientFastRise, and
    // in its Speed) take the attack, while a hit rises; two that differ in how fast they fall (in transientLongFall, and in
    // its Speed's fall) take what follows it, after a hit has passed; on a steady sound each pair agrees, so nothing turns.
    // Each part of transientWhole decibels or more gives its knob's whole amount, and one of transientUnheard or less none (a
    // sine that starts through the low cut rings a few tenths of a decibel over its level, which the long follower held for a
    // second, and a steady sound came out that much louder or quieter); its gain glides over transientGlideSeconds
    // and is kept within transientMost decibels.
    constexpr float transientFloor = -90.0f;
    constexpr float transientFastRise = 0.0002f, transientSustainRise = 0.001f, transientLongFall = 0.6f; // seconds
    constexpr float transientFallPerSpeed = 4.0f, transientFallAdded = 0.02f; // both fall in 4 times Speed, and 20 ms more
    constexpr float mostPunch = 15.0f, mostSustain = 18.0f, transientWhole = 12.0f, transientMost = 24.0f;
    constexpr float transientUnheard = 1.0f;

    // How far the place a gain is read from moves a sample, as automation moves the Look-Ahead (or the mode): an eighth of a
    // sample, so that reading towards what is newer plays the gain's own moves an eighth faster at most, and never a step.
    constexpr float curveGlide = 0.125f;
    constexpr double transientGlideSeconds = 0.001;

    Compressor::Character getCharacter (float value) noexcept
    {
        return (Compressor::Character) juce::jlimit (0, (int) std::size (characters) - 1, (int) std::lround (value));
    }

    // Drive's curve, the odd harmonics' tanh with some of the even harmonics' tanh squared: f (x) = tanh x + e tanh^2 x, and
    // its antiderivative, F (x) = ln cosh x + e (x - tanh x), written so that cosh never overflows (dsp/Saturation.h plays a
    // curve that has these two).
    struct DriveCurve
    {
        float even = 0.0f;

        float value (float x) const noexcept
        {
            const auto t = std::tanh (x);
            return t + even * t * t;
        }

        double integral (double x) const noexcept
        {
            const auto a = std::abs (x);
            return a + std::log1p (std::exp (-2.0 * a)) - 0.69314718055994531 + (double) even * (x - std::tanh (x));
        }
    };

    // How long the fade between the plain delay and the drive takes, either way: 64 samples at 44.1 kHz.
    constexpr double driveFadeSeconds = 0.00145;

    // Samples the drive plays, from silence, before it is faded in: what its half-band filters start with has rung out by
    // then, to the bit (after 64 samples it was still 53 dB under a 10 kHz tone, after 128, 94). A curve with even harmonics
    // plays longer, for the filter that takes their offset out of what it adds to settle (a 10 Hz high-pass: after 150 ms
    // it has taken out all but a thousandth of the offset; after 256 samples, less than a third of it): its offset came in with
    // the fade, a thump.
    constexpr int driveWarmUp = 256;
    constexpr double evenWarmUpSeconds = 0.15;

    // How much of the drive's travel fades its curve in from a straight line (1.8 dB into the curve): at the curve's whole
    // at once, the first touch of Drive took a full-scale peak 2.4 dB down. FET's and Tube's colour fades its curve in over
    // their first 3 dB of reduction, so that they sound as they were tuned from there on (their colour adds at most a tenth
    // of Drive's travel, all of it inside the knob's fade) and nothing jumps as they begin to turn down.
    constexpr float curveFadeDrive = 0.1f;
    constexpr float colourCurveReduction = 3.0f;
} // namespace

// A sample through the drive: its gain into the curve and back out, the curve faded in as far as its share says, the DC that
// the even harmonics leave filtered out of what the curve adds. While the share is none, and nothing has an offset to take out,
// the straight line alone: the curve's antiderivative is the costly part, and its averaging starts again as the share leaves 0.
template <typename Curve>
float Compressor::Drive::play (float sample, float gain, const Curve& curve, float share, const dsp::FilterCoefficients& dc,
                               bool blocksDc) noexcept
{
    const auto [first, second] = oversampler.up (sample);
    std::array<float, 2> played {};
    const auto curveRests = share <= 0.0f && ! blocksDc;

    if (! curveRests && std::exchange (curveRested, false))
        bent.reset();

    curveRested = curveRested || curveRests;

    for (size_t k = 0; k < 2; ++k)
    {
        const auto x = k == 0 ? first : second;
        const auto plain = straight.process (x, dsp::StraightCurve {});

        if (curveRests)
        {
            played[k] = plain;
            continue;
        }

        auto added = bent.process (x * gain, curve) / gain - plain;

        if (blocksDc)
            added = dcBlocker.process (added, dc);

        played[k] = plain + share * added;
    }

    return rest.process (oversampler.down (played[0], played[1]));
}

void Compressor::Drive::reset() noexcept
{
    oversampler.reset();
    bent.reset();
    straight.reset();
    dcBlocker.reset();
    rest.reset();
    curveRested = false;
}

int Compressor::getDriveWarmUp (float even, float share) const noexcept
{
    return even > 0.0f && share > 0.0f ? evenWarmUp : driveWarmUp;
}

int Compressor::getDriveLatency()
{
    return dsp::AlignedShaper::getLatencySamples();
}

const DeviceInfo Compressor::info { .typeID = typeID,
                                    .name = "Compressor",
                                    .kind = DeviceKind::effect,
                                    .parameters = parameters,
                                    .hasSidechain = true };

void Compressor::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    reduction.prepare (sampleRate);
    makeupSmoothing = (float) (1.0 - std::exp (-1.0 / (makeupSeconds * sampleRate)));
    keyStep = (float) std::min (1.0, 1.0 / (keyFadeSeconds * sampleRate));
    rmsSmoothing = (float) (1.0 - std::exp (-1.0 / (rmsSeconds * sampleRate)));
    memoryRise = (float) (1.0 - std::exp (-1.0 / (memoryRiseSeconds * sampleRate)));
    memoryFall = (float) (1.0 - std::exp (-1.0 / (memoryFallSeconds * sampleRate)));
    const auto samplesPerPeak = std::max (1, (int) std::lround (sampleRate * 0.001));
    keyPeak.samplesPerPeak = samplesPerPeak;
    levelPeak.samplesPerPeak = samplesPerPeak;
    transientPeak.samplesPerPeak = samplesPerPeak;
    transientGlide = (float) (1.0 - std::exp (-1.0 / (transientGlideSeconds * sampleRate)));

    for (auto* follower : { &fastFollower, &slowFollower, &longFollower, &shortFollower })
        follower->prepare (sampleRate);

    driveLatency = getDriveLatency();
    latency = getLatencyFor (preparedMode, preparedLookahead);

    for (auto* delays : { &soundDelays, &keyDelays, &dryDelays, &dryRests })
        for (auto& delay : *delays)
            delay.prepare (std::max (1, latency));

    for (auto& delay : undriven)
        delay.prepare (std::max (1, driveLatency));

    for (auto* set : { &drives, &nextDrives })
        for (auto& each : *set)
            each.rest.setDelay ((float) driveLatency - dsp::AntialiasedShaper::getLatencySamples());

    driveBlendStep = (float) std::min (1.0, 1.0 / (driveFadeSeconds * sampleRate));
    evenWarmUp = std::max (driveWarmUp, (int) std::lround (evenWarmUpSeconds * sampleRate));
    for (auto* delay : { &gainDelay, &curveGainDelay, &reductionDelay, &makeupDelay })
        delay->prepare (std::max (1, latency));

    prepared = true;
    reset();
}

int Compressor::getLatencyFor (Mode shownMode, float lookaheadMilliseconds) const noexcept
{
    const auto driven = getDriveLatency();

    switch (shownMode)
    {
        case Mode::duck:
            return std::max (driven, (int) std::lround (duckLookaheadSeconds * sampleRate));
        case Mode::compress:
            return driven + (int) std::lround (std::clamp (lookaheadMilliseconds, 0.0f, mostLookaheadMilliseconds) * 0.001 * sampleRate);
        case Mode::transient:
            break;
    }

    return driven;
}

int Compressor::getLatencySamplesFor (std::span<const float> values) const
{
    if (values.size() <= (size_t) lookahead)
        return getDriveLatency();

    return getLatencyFor (getMode (values[(size_t) mode]), values[(size_t) lookahead]);
}

void Compressor::takeValuesBeforePrepare (std::span<const float> values)
{
    preparedMode = values.size() > (size_t) mode ? getMode (values[(size_t) mode]) : Mode::compress;
    preparedLookahead = values.size() > (size_t) lookahead ? values[(size_t) lookahead] : 0.0f;
}

void Compressor::reset()
{
    reduction.reset();

    keyPeak.reset();
    levelPeak.reset();
    transientPeak.reset();
    lastKeyPeak = -1.0f;
    keyDecibels = -120.0f;
    ducked = 0.0f;
    lastDucked = -1.0f;
    duckGain = 1.0f;
    sinceHit = mostSinceHit;
    sinceUnder = mostSinceHit;
    releaseFrom = 0.0f;
    releaseAlong = 0.0;
    releasing = false;
    wasOver = false;

    for (auto* filters : { &lowCuts, &transientCuts })
        for (auto& filter : *filters)
            filter.reset();

    for (auto* follower : { &fastFollower, &slowFollower, &longFollower, &shortFollower })
        follower->reset (transientFloor);

    transientDecibels = 0.0f;
    transientLevel = transientFloor;
    lastMode = Mode::compress;
    playedYet = false;

    for (auto& channel : highCuts)
        for (auto& filter : channel)
            filter.reset();

    for (auto* delays : { &soundDelays, &keyDelays, &dryDelays, &dryRests, &undriven })
        for (auto& delay : *delays)
            delay.reset();

    for (auto* set : { &drives, &nextDrives })
        for (auto& each : *set)
            each.reset();

    for (auto& shaper : dryDrives)
        shaper.reset();

    driveBlend = 0.0f;
    curveShare = 0.0f;
    knobShare = 0.0f;
    lastDriveAmount = -1.0f;
    drivesRan = false;
    driveWarm = 0;
    warmUp = driveWarmUp;
    isFresh = true;
    playingEven = 0.0f;
    changesCurve = false;
    nextBlend = 0.0f;
    clearGainDelays();
    curveLate = -1.0f;
    duckLate = latency;
    heldCurveSamples = 0;
    heldDuckSamples = 0;
    freshPending = false;
    freshSamples = 0;
    transientHeld = 0.0f;
    isFirstBlock = true;
    keyShare = 0.0f;
    meanSquare = 0.0f;
    releaseStart = 0.0f;
    memory = 0.0f;
    lastLevel = -1.0f;
    lastWanted = 0.0f;
    colourAmount = 0.0f;
}

void Compressor::clearGainDelays() noexcept
{
    // None, unity, in every place a mode reads from, so that nothing reads a gain of silence; Compress's reduction none, and
    // its makeup unity.
    reductionDelay.reset();

    if (! prepared)
        return;

    for (int i = 0; i <= latency + 3; ++i)
    {
        gainDelay.push (1.0f);
        curveGainDelay.push (1.0f);
        makeupDelay.push (1.0f);
    }
}

void Compressor::startGlides (std::span<const float> values, float targetMakeup) noexcept
{
    makeupDecibels = targetMakeup;
    inputDecibels = values[inputGain];
    driveAmount = values[drive];
    knobShare = std::clamp (values[drive] / curveFadeDrive, 0.0f, 1.0f);
    curveShare = knobShare;
    isFirstBlock = false;
}

float Compressor::hearLevel (float left, float right, const dsp::FilterCoefficients& lowCut) noexcept
{
    const auto l = transientCuts[0].process (left, lowCut), r = transientCuts[1].process (right, lowCut);
    transientHeard = { l, r };
    transientHeld = transientPeak.process (std::max (std::abs (l), std::abs (r)), keyPeakMilliseconds);
    return transientHeld;
}

void Compressor::startFollowers() noexcept
{
    transientLevel = std::max (transientFloor, dsp::gainToDecibels (transientHeld));

    for (auto* follower : { &fastFollower, &slowFollower, &longFollower, &shortFollower })
        follower->reset (transientLevel);
}

void Compressor::hearTransients (float left, float right, const dsp::FilterCoefficients& lowCut) noexcept
{
    transientLevel = std::max (transientFloor, dsp::gainToDecibels (hearLevel (left, right, lowCut)));

    for (auto* follower : { &fastFollower, &slowFollower, &longFollower, &shortFollower })
        follower->process (transientLevel);
}

void Compressor::handOver (Mode to, std::span<const float> values) noexcept
{
    // The gain goes on from where the mode before left it, so that the sound doesn't jump, each mode then moving as its own
    // times say: in decibels, Compress's is its makeup less its reduction, Duck's its duck down, Transient's its makeup and
    // its own.
    const auto from = lastMode;
    const auto gainBefore = from == Mode::duck       ? -ducked
                            : from == Mode::compress ? makeupDecibels - reduction.getValue()
                                                     : makeupDecibels + transientDecibels;

    switch (to)
    {
        case Mode::compress:
            reduction.reset (makeupDecibels - gainBefore);
            break;
        case Mode::transient:
            transientDecibels = gainBefore - makeupDecibels;
            startFollowers();
            break;
        case Mode::duck:
            ducked = std::clamp (-gainBefore, 0.0f, std::max (0.0f, values[depth]));
            releasing = false;
            break;
    }

    // What is on its way through the first delay, or the drive's, was turned, or is to be, by the mode before: its gain for
    // what it would have looked ahead at is the one it had as it changed (the mode playing now pushes it for as long as that
    // look-ahead, then none). Compress's and Transient's gain is turned as the sound leaves the first delay; Duck's as it
    // leaves the drive's.
    if (from == Mode::duck)
    {
        heldDuckGain = duckGain;
        heldDuckSamples = latency - duckLate;
        heldCurveSamples = 0;
    }
    else if (to == Mode::duck)
    {
        heldCurveGain = lastGain;
        heldCurveSamples = curveLate >= 0.0f ? (latency - driveLatency) - (int) std::floor (curveLate) : 0;
        heldDuckSamples = 0;
    }

    // And the gain the new mode works out for what it looks ahead at, as it starts, is for what the mode before turns: none,
    // for as long as that look-ahead (freshSamples), so that nothing is turned by both.
    freshPending = (from == Mode::duck) != (to == Mode::duck);
}

void Compressor::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    silenceWhatIsNotSound (block);
    const auto& values = block.parameters;
    const auto shownMode = getMode (values[mode]);

    // Transient's followers: the fast one rises at once and the slow one in its Speed, both falling in four times that and
    // 20 ms more; the long and short ones rise alike, the long one falling over 600 ms, the short one as the other two.
    const auto speedSeconds = values[speed] * 0.001f;
    const auto fall = transientFallPerSpeed * speedSeconds + transientFallAdded;
    fastFollower.setTimes (transientFastRise, fall);
    slowFollower.setTimes (speedSeconds, fall);
    longFollower.setTimes (transientSustainRise, transientLongFall);
    shortFollower.setTimes (transientSustainRise, fall);

    if (playedYet && shownMode != lastMode)
        handOver (shownMode, values);

    lastMode = shownMode;
    playedYet = true;

    // Transient's ear hears the level in every mode (two filters and a comparison), so that it has it when it switches to
    // Transient; its followers run in Transient alone.
    if (shownMode != Mode::transient && block.numChannels > 0)
    {
        const auto lowCut = dsp::makeFilter (dsp::FilterShape::highPass, values[keyLowCut], sampleRate, 0.70710678);
        const auto* left = block.channels[0];
        const auto* right = block.numChannels > 1 ? block.channels[1] : left;

        for (int i = 0; i < block.numSamples; ++i)
            hearLevel (left[i], right[i], lowCut);
    }

    switch (shownMode)
    {
        case Mode::duck:
            duck (block);
            break;
        case Mode::transient:
            shapeTransients (block);
            break;
        case Mode::compress:
            compress (block);
            break;
    }

    isFresh = false;
}

void Compressor::compress (const ProcessContext& block)
{
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);
    const auto type = getCharacter (values[character]);

    // What the detector hears: the track its sidechain listens to (its key), as it comes in, while one feeds it, as a keyed
    // compressor hears; else the sound itself as it comes in, after the input gain, which doesn't drive a key. Through the
    // key's low cut, as a bus compressor's sidechain high-pass keeps the bass from pumping it, and its high cut.
    const float* const* heard = block.channels;
    auto numHeard = numChannels;
    const auto keyed = block.sidechain != nullptr && block.numSidechainChannels > 0;

    if (keyed)
    {
        heard = block.sidechain;
        numHeard = std::min (block.numSidechainChannels, 2);
    }

    // A curve changed: what a level heard wants is worked out again, though the level heard is the same.
    if (gains.update (getCurve (values)))
        lastLevel = -1.0f;

    const auto targetMakeup = values[makeup] + (values[autoMakeup] >= 0.5f ? getAutoMakeupDecibels (gains) : 0.0f);
    const auto targetInput = values[inputGain];
    const auto hearsAverage = type == Character::vca || type == Character::opto || type == Character::tube;
    const auto lowCut = dsp::makeFilter (dsp::FilterShape::highPass, values[keyLowCut], sampleRate, 0.70710678);
    const auto cutsHigh = values[keyHighCut] < highCutOff;
    const std::array highCut { dsp::makeFilter (dsp::FilterShape::lowPass, values[keyHighCut], sampleRate, dsp::getButterworthQ (4, 0)),
                               dsp::makeFilter (dsp::FilterShape::lowPass, values[keyHighCut], sampleRate, dsp::getButterworthQ (4, 1)) };
    const auto keyWanted = isPlayingKey() ? 1.0f : 0.0f;
    const auto colourOf = colourShares[(size_t) type];
    const auto even = evenShares[(size_t) type];
    const auto dcBlocker = dsp::makeFilter (dsp::FilterShape::highPass, dcBlockerFrequency, 2.0 * sampleRate, 0.70710678);
    reduction.setTimes (values[attack] * 0.001f, values[release] * 0.001f);

    // Its Hold: the detector hears the loudest level over the last so many milliseconds (none at 0, which plays as without).
    const auto holdMilliseconds =
        values[levelHold] > 0.0f ? std::clamp ((int) std::ceil (values[levelHold]), 1, mostLevelHoldMilliseconds) : 0;

    // Opto's slow stage, after its first half: slower still the longer it has held lately.
    const auto slowRelease = values[release] * 0.001f * (opticalSlowdown + opticalMemorySlowdown * memory);
    opticalCoefficient = (float) std::exp (-1.0 / ((double) slowRelease * sampleRate));

    // How far the drive's curve is in: the knob's share over its first tenth, or FET's and Tube's colour's over their first
    // 3 dB of reduction, the more of the two; the knob's glides, and the colour's follows how far it turns down, smoothly,
    // sample by sample, so that it plays the same however its blocks are cut.
    const auto knobShareWanted = std::clamp (values[drive] / curveFadeDrive, 0.0f, 1.0f);

    if (isFirstBlock)
        startGlides (values, targetMakeup);

    // Whether the sound goes through the drive: while its knob is up or the character has a colour of its own, whether or not
    // that colours the sound now (the straight line alone meanwhile), and until a drive turned off has faded out. Coming on,
    // it starts from silence, plays a while for what its filters start with to ring out (longer for a curve with an offset to
    // take out, unless the curve's share starts at none, so that the filter settles as it comes in), and is faded in; from a
    // reset, with silence in all of it, at once. Another curve, while the drive is heard, warms up beside the one playing,
    // on the same sound, and the sound fades from one to the other: it stays driven all through, and no offset comes in.
    const auto shaped = values[drive] > 0.0f || colourOf > 0.0f || driveAmount > 1.0e-4f;
    const auto drivesRun = shaped || driveBlend > 0.0f;

    if (drivesRun && ! drivesRan)
    {
        for (auto& each : drives)
            each.reset();

        for (auto& shaper : dryDrives)
            shaper.reset();

        driveWarm = 0;
        warmUp = getDriveWarmUp (even, curveShare);
        playingEven = even;
        changesCurve = false;
    }

    if (isFresh)
    {
        driveBlend = shaped ? 1.0f : 0.0f;
        playingEven = even;
        warmUp = getDriveWarmUp (even, curveShare);
        driveWarm = warmUp;
        changesCurve = false;
    }

    drivesRan = drivesRun;

    // A curve still warming that is no longer wanted is dropped; one fading in fades in whole, and another is warmed after.
    if (changesCurve && nextBlend <= 0.0f && ! juce::exactlyEqual (even, nextEven))
        changesCurve = false;

    if (! juce::exactlyEqual (even, playingEven) && ! changesCurve)
    {
        if (driveBlend > 0.0f)
        {
            for (auto& each : nextDrives)
                each.reset();

            nextEven = even;
            nextWarm = 0;
            nextWarmUp = getDriveWarmUp (even, curveShare);
            nextBlend = 0.0f;
            changesCurve = true;
        }
        else
        {
            // Not heard: its averaging starts again, its offset's filter from nothing, and its warm-up from the start.
            for (auto& each : drives)
            {
                each.bent.restartAveraging();
                each.dcBlocker.reset();
            }

            playingEven = even;
            driveWarm = 0;
            warmUp = getDriveWarmUp (even, curveShare);
        }
    }

    DriveCurve curve { playingEven };
    const DriveCurve nextCurve { nextEven };

    // The sound, after the input gain, goes through a delay for the rest of the latency over the drive's (its Look-Ahead, or
    // Duck's look-ahead while automation has it compress), and is turned by the gain the detector worked out as many samples
    // after it less the Look-Ahead: so that the gain is down as a peak the detector heard ahead plays. The copy of what came in
    // for the mix goes through the same delay, then through filters like the drive's with the sound, while the drive plays,
    // so that they are ready whenever the mix leaves 100 %.
    const auto rest = latency - driveLatency;
    const auto ahead = std::clamp ((int) std::lround (values[lookahead] * 0.001 * sampleRate), 0, rest);
    const auto targetLate = (float) (rest - ahead);

    if (curveLate < 0.0f)
        curveLate = targetLate;

    if (std::exchange (freshPending, false))
        freshSamples = rest - (int) std::floor (curveLate);
    const auto turnsDry = block.dry != nullptr && drivesRun;

    // The gain is worked out again only when it changes (docs/archive/performance-audit.md, A5).
    auto loudestPeak = 0.0f, most = 0.0f, least = 0.0f;

    for (int i = 0; i < block.numSamples; ++i)
    {
        // The input gain, gliding to where it is set.
        inputDecibels += (targetInput - inputDecibels) * makeupSmoothing;

        if (! juce::exactlyEqual (inputDecibels, lastInputDecibels))
        {
            lastInputDecibels = inputDecibels;
            lastInput = dsp::decibelsToGain (inputDecibels);
        }

        // What it hears, through its cuts.
        auto peak = 0.0f, squares = 0.0f;
        std::array<float, 2> key {};

        for (int channel = 0; channel < numHeard; ++channel)
        {
            auto& cut = highCuts[(size_t) channel];
            auto x = lowCuts[(size_t) channel].process (keyed ? soundOf (heard[channel][i]) : heard[channel][i] * lastInput, lowCut);

            if (cutsHigh)
                x = cut[1].process (cut[0].process (x, highCut[0]), highCut[1]);

            key[(size_t) channel] = x;
            peak = std::max (peak, std::abs (x));
            squares = std::max (squares, x * x);
        }

        // An average ear hears the mean square, as loud as a sine's peak is for a steady sine (twice its mean square).
        auto level = peak;

        if (hearsAverage)
        {
            meanSquare += (squares - meanSquare) * rmsSmoothing;
            level = std::sqrt (2.0f * meanSquare);
        }

        // Held: the loudest it heard over the Hold, as Duck's ear holds its key's, so that its reduction stays down that long
        // after a peak, and a curve that gates stays open that long after the sound falls under it.
        if (const auto held = levelPeak.process (level, std::max (1, holdMilliseconds)); holdMilliseconds > 0)
            level = held;

        // How far its curve turns the level down (below 0, up), worked out again only when the level changes.
        if (! juce::exactlyEqual (level, lastLevel))
        {
            lastLevel = level;
            lastWanted = -gains.getDecibels (level);
        }

        const auto wanted = lastWanted;
        const auto previous = reduction.getValue();
        auto amount = reduction.process (wanted);

        // Opto lets go of a reduction at the release set for the first half of the way, as the LA-2A's cell does in its first
        // 60 ms, then slowly.
        if (type == Character::opto)
        {
            if (amount >= previous)
                releaseStart = amount; // gripping, or holding: where a release would start from
            else if (previous > 0.0f && previous < 0.5f * releaseStart)
                amount = wanted + (previous - wanted) * opticalCoefficient;

            reduction.reset (amount);
            const auto holds = amount > memoryHold;
            memory += ((holds ? 1.0f : 0.0f) - memory) * (holds ? memoryRise : memoryFall);
        }

        makeupDecibels += (targetMakeup - makeupDecibels) * makeupSmoothing;
        driveAmount += (values[drive] - driveAmount) * makeupSmoothing;

        if (const auto gainDecibels = makeupDecibels - amount; ! juce::exactlyEqual (gainDecibels, lastGainDecibels))
        {
            lastGainDecibels = gainDecibels;
            lastGain = dsp::decibelsToGain (gainDecibels);
        }

        if (! juce::exactlyEqual (makeupDecibels, lastMakeupDecibels))
        {
            lastMakeupDecibels = makeupDecibels;
            makeupGain = dsp::decibelsToGain (makeupDecibels);
        }

        // The gain for the sound as late as the Look-Ahead leaves it, the place it is read from gliding to where the Look-Ahead
        // puts it; and as late, its reduction, which FET's and Tube's colour follows, and its makeup, which the drive takes off
        // and puts back, so that each is the one worked out for the sound it turns.
        curveLate += std::clamp (targetLate - curveLate, -curveGlide, curveGlide);
        const auto pushed = takeFresh() ? 1.0f : lastGain;
        curveGainDelay.push (pushed);
        reductionDelay.push (amount);
        makeupDelay.push (makeupGain);
        const auto isLate = curveLate > 0.0f;
        const auto gain = isLate ? curveGainDelay.read (curveLate) : pushed;
        const auto lateAmount = isLate ? reductionDelay.read (curveLate) : amount;
        const auto lateMakeup = isLate ? makeupDelay.read (curveLate) : makeupGain;
        colourAmount += (std::max (0.0f, lateAmount) - colourAmount) * makeupSmoothing;

        // The sound, turned down and driven into its curve (the makeup after it); or what it hears, to listen to, faded in and
        // out.
        keyShare += std::clamp (keyWanted - keyShare, -keyStep, keyStep);

        // Duck's gain, which it plays late, is none for what goes through now (but for what Duck worked out for the sound on its
        // way, should it have ducked before): should automation have it duck next, it turns what is still on its way no further
        // down. What leaves the drive's delay is turned by it.
        gainDelay.push (takeHeldDuckGain());
        const auto duckedBefore = gainDelay.read (duckLate);

        // The drive: the knob's, and FET's and Tube's colour's as far as they turn down.
        const auto colouring = std::min (1.0f, colourAmount / colourReduction);
        const auto drivenBy = std::min (1.0f, driveAmount + colourOf * colouring);

        if (! juce::exactlyEqual (drivenBy, lastDriveAmount))
        {
            lastDriveAmount = drivenBy;
            driveGain = dsp::decibelsToGain (drivenBy * mostDrive);
        }

        knobShare += (knobShareWanted - knobShare) * makeupSmoothing;

        if (knobShareWanted <= 0.0f && knobShare < 1.0e-6f)
            knobShare = 0.0f;

        curveShare = colourOf > 0.0f ? std::max (knobShare, std::min (1.0f, colourAmount / colourCurveReduction)) : knobShare;

        // Faded in once it has warmed up; and another curve, once it has, faded to.
        driveWarm = std::min (driveWarm + (drivesRun ? 1 : 0), warmUp);
        const auto blendTarget = shaped && driveWarm >= warmUp ? 1.0f : 0.0f;
        driveBlend += std::clamp (blendTarget - driveBlend, -driveBlendStep, driveBlendStep);

        if (changesCurve)
        {
            nextWarm = std::min (nextWarm + 1, nextWarmUp);

            if (nextWarm >= nextWarmUp)
                nextBlend = std::min (1.0f, nextBlend + driveBlendStep);
        }

        for (int channel = 0; channel < numChannels; ++channel)
        {
            // The sound, as late as the rest of the latency, turned down, and as late again as the drive plays it.
            const auto c = (size_t) channel;
            auto in = block.channels[channel][i] * lastInput;

            if (rest > 0)
            {
                soundDelays[c].push (in);
                in = soundDelays[c].read (rest);
            }

            const auto turnedDown = in * gain;
            undriven[c].push (turnedDown);
            auto sound = undriven[c].read (driveLatency);

            if (drivesRun)
            {
                // Without the makeup, into the curve and back out at the level it came in at, as far as the drive has faded
                // the curve in; then the makeup. While nothing drives it, the straight line alone, through the same filters.
                const auto plain = turnedDown / lateMakeup;
                auto y = drives[c].play (plain, driveGain, curve, curveShare, dcBlocker, playingEven > 0.0f);

                if (changesCurve)
                    y += nextBlend * (nextDrives[c].play (plain, driveGain, nextCurve, curveShare, dcBlocker, nextEven > 0.0f) - y);

                sound += driveBlend * (y * lateMakeup - sound);
            }

            sound *= duckedBefore;

            // What it hears, as late, to listen to.
            keyDelays[c].push (key[(size_t) std::min (channel, numHeard - 1)]);
            block.channels[channel][i] = sound + keyShare * (keyDelays[c].read (latency) - sound);

            // The copy of what came in, as late, the same way; and turned as the drive's filters turn the sound.
            if (block.dry != nullptr)
            {
                auto& came = block.dry[channel][i];
                auto early = came;

                if (rest > 0)
                {
                    dryRests[c].push (early);
                    early = dryRests[c].read (rest);
                }

                dryDelays[c].push (early);
                auto late = dryDelays[c].read (driveLatency);

                if (turnsDry)
                    late += driveBlend * (dryDrives[c].process (early, dsp::StraightCurve {}) - late);

                came = late;
            }
        }

        // Faded to the other curve whole: it plays on.
        if (changesCurve && nextBlend >= 1.0f)
        {
            std::swap (drives, nextDrives);
            playingEven = nextEven;
            curve.even = nextEven;
            changesCurve = false;
            nextBlend = 0.0f;
        }

        loudestPeak = std::max (loudestPeak, level);
        most = std::max (most, amount);
        least = std::min (least, amount);
    }

    levelMeter.raise (dsp::gainToDecibels (loudestPeak));
    reductionMeter.raise (most);
    boostMeter.raise (-least);
    gainReduction.push (most);
}

void Compressor::duck (const ProcessContext& block)
{
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);

    // What the detector hears: the key, the track the sidechain listens to, else the sound itself, as each comes in.
    const float* const* heard = block.channels;
    auto numHeard = numChannels;

    if (block.sidechain != nullptr && block.numSidechainChannels > 0)
    {
        heard = block.sidechain;
        numHeard = std::min (block.numSidechainChannels, 2);
    }

    const auto lowCut = dsp::makeFilter (dsp::FilterShape::highPass, values[keyLowCut], sampleRate, 0.70710678);
    const auto cutsHigh = values[keyHighCut] < highCutOff;
    const std::array highCut { dsp::makeFilter (dsp::FilterShape::lowPass, values[keyHighCut], sampleRate, dsp::getButterworthQ (4, 0)),
                               dsp::makeFilter (dsp::FilterShape::lowPass, values[keyHighCut], sampleRate, dsp::getButterworthQ (4, 1)) };
    const auto keyWanted = isPlayingKey() ? 1.0f : 0.0f;

    // The whole depth over the attack, a straight line in decibels (at once without one); held at least the hold from a hit;
    // then back over the release, its way bent by the release curve.
    const auto mostDucked = std::max (0.0f, values[depth]);
    const auto attackSamples = (double) values[attack] * 0.001 * sampleRate;
    const auto attackStep = attackSamples >= 1.0 ? (float) (mostDucked / attackSamples) : mostDucked;

    // Looking ahead: the gain worked out from the key now turns the sound that comes this much later, by the attack, or by the
    // whole look-ahead if the attack is longer, so that it is down as the hit plays; its hold, counted from the key's hit, is
    // as much longer, and it lets go only once the key has been under its threshold as long, so that in what is heard it
    // still holds from the hit and while the key is over.
    // (Prepared to compress, a mode that automation changed, it doesn't look ahead: its sound and its gain as late alike.)
    const auto ahead = std::clamp ((int) std::ceil (attackSamples), 0, preparedMode == Mode::duck ? latency : 0);
    const auto gainLate = latency - ahead;
    const auto rest = latency - driveLatency; // what the sound is late by after the drive's own delay
    duckLate = gainLate;

    if (std::exchange (freshPending, false))
        freshSamples = ahead;
    const auto holdSamples =
        (int) std::min ((double) mostSinceHit, std::max (0.0, (double) values[hold] * 0.001 * sampleRate) + (double) ahead);
    const auto releaseStep = 1.0 / std::max (1.0, (double) values[release] * 0.001 * sampleRate);
    const auto curve = (double) juce::jlimit (-1.0f, 1.0f, values[releaseCurve]);
    const auto thresholdDecibels = values[Compressor::threshold];

    // Compress's drive starts again, from silence, when it compresses again.
    drivesRan = false;
    driveBlend = 0.0f;
    changesCurve = false;
    nextBlend = 0.0f;
    auto loudestKey = 0.0f, most = 0.0f;

    for (int i = 0; i < block.numSamples; ++i)
    {
        // The key, through its cuts, and its loudest over the last few milliseconds.
        auto peak = 0.0f;
        std::array<float, 2> key {};

        for (int channel = 0; channel < numHeard; ++channel)
        {
            auto& cut = highCuts[(size_t) channel];
            auto x = lowCuts[(size_t) channel].process (soundOf (heard[channel][i]), lowCut);

            if (cutsHigh)
                x = cut[1].process (cut[0].process (x, highCut[0]), highCut[1]);

            key[(size_t) channel] = x;
            peak = std::max (peak, std::abs (x));
        }

        if (const auto level = keyPeak.process (peak, keyPeakMilliseconds); ! juce::exactlyEqual (level, lastKeyPeak))
        {
            lastKeyPeak = level;
            keyDecibels = dsp::gainToDecibels (level);
        }

        // How far it wants the sound down: the whole depth 6 dB over the threshold, a share of it less far over. A hit is
        // the key rising over the threshold.
        const auto over = keyDecibels > thresholdDecibels;
        const auto wanted = over ? mostDucked * std::min (1.0f, (keyDecibels - thresholdDecibels) / duckKnee) : 0.0f;

        if (over && ! wasOver)
        {
            sinceHit = 0;
            releasing = false;
        }

        wasOver = over;
        sinceUnder = over ? 0 : std::min (sinceUnder + 1, mostSinceHit);

        if (wanted > ducked)
        {
            // Gripping, from where it is.
            ducked = std::min (wanted, ducked + attackStep);
            releasing = false;
        }
        else if (! releasing && ! over && sinceUnder > ahead && ducked > 0.0f && sinceHit >= holdSamples)
        {
            // Held long enough, and the key under the threshold: letting go, from where it is.
            releasing = true;
            releaseFrom = ducked;
            releaseAlong = 0.0;
        }

        if (releasing)
        {
            releaseAlong = std::min (1.0, releaseAlong + releaseStep);
            ducked = releaseFrom * (float) (1.0 - dsp::getEasedProgress (releaseAlong, curve));

            if (releaseAlong >= 1.0)
            {
                releasing = false;
                ducked = 0.0f;
            }
        }

        sinceHit = std::min (sinceHit + 1, mostSinceHit);

        if (! juce::exactlyEqual (ducked, lastDucked))
        {
            lastDucked = ducked;
            duckGain = dsp::decibelsToGain (-ducked);
        }

        // The sound, turned down, as late as the look-ahead; or what it hears, to listen to, as late, faded in and out.
        keyShare += std::clamp (keyWanted - keyShare, -keyStep, keyStep);
        auto gain = duckGain;

        if (latency > 0)
        {
            gainDelay.push (takeFresh() ? 1.0f : duckGain);
            gain = gainDelay.read (gainLate);
        }

        // Compress's and Transient's gain, which they play late, is none for what goes through now (but for the gain they had
        // as Duck took over, for what they would have looked ahead at); what leaves the first delay, which they would have
        // turned, is turned by the gain they worked out for it.
        curveGainDelay.push (takeHeldCurveGain());
        reductionDelay.push (0.0f);
        makeupDelay.push (makeupGain);
        const auto curvedBefore = curveLate >= 0.0f ? curveGainDelay.read (curveLate) : 1.0f;

        for (int channel = 0; channel < numChannels; ++channel)
        {
            // The sound through the delays Compress sends it through, one for the rest of the latency and then the drive's, so
            // that a change of mode plays on what is on its way through them: nothing lost, nothing played again.
            const auto c = (size_t) channel;
            auto sound = block.channels[channel][i];

            if (rest > 0)
            {
                soundDelays[c].push (sound);
                sound = soundDelays[c].read (rest);
                sound *= curvedBefore;
            }

            undriven[c].push (sound);
            sound = undriven[c].read (driveLatency);

            keyDelays[c].push (key[(size_t) std::min (channel, numHeard - 1)]);
            const auto keyHeard = keyDelays[c].read (latency);
            const auto turnedDown = sound * gain;
            block.channels[channel][i] = turnedDown + keyShare * (keyHeard - turnedDown);

            // The copy of what came in for the mix, as late as the sound and the same way (as Compress sends it), so that a
            // change of mode plays on what is on its way.
            if (block.dry != nullptr)
            {
                auto& came = block.dry[channel][i];
                auto late = came;

                if (rest > 0)
                {
                    dryRests[c].push (late);
                    late = dryRests[c].read (rest);
                }

                dryDelays[c].push (late);
                came = dryDelays[c].read (driveLatency);
            }
        }

        loudestKey = std::max (loudestKey, lastKeyPeak);
        most = std::max (most, ducked);
    }

    levelMeter.raise (dsp::gainToDecibels (loudestKey));
    reductionMeter.raise (most);
    gainReduction.push (most);
    colourAmount = 0.0f; // Compress's colour, which nothing turned down for
}

void Compressor::shapeTransients (const ProcessContext& block)
{
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);

    if (numChannels <= 0)
        return;

    // It hears its own sound, through the key's low cut, never a key; and turns both channels alike by its punch over the
    // attack and its sustain over what follows it, each part of 12 dB or more giving the whole amount, and its makeup.
    const auto lowCut = dsp::makeFilter (dsp::FilterShape::highPass, values[keyLowCut], sampleRate, 0.70710678);
    const auto targetMakeup = values[makeup];
    const auto punchDecibels = std::clamp (values[punch], -1.0f, 1.0f) * mostPunch;
    const auto sustainDecibels = std::clamp (values[sustain], -1.0f, 1.0f) * mostSustain;
    const auto keyWanted = isPlayingKey() ? 1.0f : 0.0f;
    const auto rest = latency - driveLatency; // its sound as late as Compress's, with no look-ahead

    if (curveLate < 0.0f)
        curveLate = (float) rest;

    if (std::exchange (freshPending, false))
        freshSamples = rest - (int) std::floor (curveLate);
    const auto share = [] (float part) { return std::clamp ((part - transientUnheard) / (transientWhole - transientUnheard), 0.0f, 1.0f); };

    if (isFirstBlock)
        startGlides (values, targetMakeup);

    // Compress's drive starts again, from silence, when it compresses again.
    drivesRan = false;
    driveBlend = 0.0f;
    changesCurve = false;
    nextBlend = 0.0f;
    colourAmount = 0.0f;

    const auto* left = block.channels[0];
    const auto* right = numChannels > 1 ? block.channels[1] : left;
    auto loudest = transientFloor, most = 0.0f, least = 0.0f;

    for (int i = 0; i < block.numSamples; ++i)
    {
        hearTransients (left[i], right[i], lowCut);

        // The attack: how far the fast follower is over the slow one, while a hit rises; what follows it: how far the long
        // follower is over the short one, after a hit has passed. Both nothing on a steady sound, at any level.
        const auto attackPart = fastFollower.getValue() - slowFollower.getValue();
        const auto sustainPart = longFollower.getValue() - shortFollower.getValue();
        const auto wanted =
            std::clamp (punchDecibels * share (attackPart) + sustainDecibels * share (sustainPart), -transientMost, transientMost);
        transientDecibels += (wanted - transientDecibels) * transientGlide;
        makeupDecibels += (targetMakeup - makeupDecibels) * makeupSmoothing;

        if (const auto gainDecibels = makeupDecibels + transientDecibels; ! juce::exactlyEqual (gainDecibels, lastGainDecibels))
        {
            lastGainDecibels = gainDecibels;
            lastGain = dsp::decibelsToGain (gainDecibels);
        }

        if (! juce::exactlyEqual (makeupDecibels, lastMakeupDecibels))
        {
            lastMakeupDecibels = makeupDecibels;
            makeupGain = dsp::decibelsToGain (makeupDecibels);
        }

        // As late as Compress turns its sound, the place it is read from gliding there from Compress's, so that a change of
        // mode plays on what is on its way; and what leaves the drive's delay turned by what Duck worked out for it, should it
        // have ducked before.
        curveLate += std::clamp ((float) rest - curveLate, -curveGlide, curveGlide);
        const auto pushed = takeFresh() ? 1.0f : lastGain;
        curveGainDelay.push (pushed);
        reductionDelay.push (0.0f);
        makeupDelay.push (makeupGain);
        const auto gain = curveLate > 0.0f ? curveGainDelay.read (curveLate) : pushed;
        gainDelay.push (takeHeldDuckGain());
        const auto duckedBefore = gainDelay.read (duckLate);
        keyShare += std::clamp (keyWanted - keyShare, -keyStep, keyStep);

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const auto c = (size_t) channel;
            auto sound = block.channels[channel][i];

            if (rest > 0)
            {
                soundDelays[c].push (sound);
                sound = soundDelays[c].read (rest);
            }

            undriven[c].push (sound * gain);
            sound = undriven[c].read (driveLatency) * duckedBefore;
            keyDelays[c].push (transientHeard[c]);
            block.channels[channel][i] = sound + keyShare * (keyDelays[c].read (latency) - sound);

            // The copy of what came in for the mix, as late as the sound and the same way.
            if (block.dry != nullptr)
            {
                auto& came = block.dry[channel][i];
                auto late = came;

                if (rest > 0)
                {
                    dryRests[c].push (late);
                    late = dryRests[c].read (rest);
                }

                dryDelays[c].push (late);
                came = dryDelays[c].read (driveLatency);
            }
        }

        loudest = std::max (loudest, transientLevel);
        most = std::max (most, -transientDecibels);
        least = std::min (least, -transientDecibels);
    }

    levelMeter.raise (loudest);
    reductionMeter.raise (most);
    boostMeter.raise (-least);
    gainReduction.push (most);
}
} // namespace phazite::devices
