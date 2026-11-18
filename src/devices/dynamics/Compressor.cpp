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
    constexpr std::string_view modes[] = { "Compress", "Duck" };
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
        { .id = "release",
          .name = "Release",
          .minimum = 5.0f,
          .maximum = 2000.0f,
          .defaultValue = 120.0f,
          .unit = Unit::milliseconds,
          .midpoint = 100.0f },
        { .id = "releaseCurve", .name = "Release Curve", .minimum = -1.0f, .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::percent },
        { .id = "makeup", .name = "Makeup", .minimum = -12.0f, .maximum = 24.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
        { .id = "drive", .name = "Drive", .defaultValue = 0.0f, .unit = Unit::percent },
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
    static_assert (std::size (modes) == (size_t) Compressor::Mode::duck + 1);
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
    samplesPerPeak = std::max (1, (int) std::lround (sampleRate * 0.001));
    driveLatency = getDriveLatency();
    latency = ducksAsPrepared ? std::max (driveLatency, (int) std::lround (duckLookaheadSeconds * sampleRate)) : driveLatency;

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
    gainDelay.prepare (std::max (1, latency));
    reset();
}

int Compressor::getLatencySamplesFor (std::span<const float> values) const
{
    const auto ducks = values.size() > (size_t) mode && getMode (values[(size_t) mode]) == Mode::duck;
    return ducks ? std::max (getDriveLatency(), (int) std::lround (duckLookaheadSeconds * sampleRate)) : getDriveLatency();
}

void Compressor::takeValuesBeforePrepare (std::span<const float> values)
{
    ducksAsPrepared = values.size() > (size_t) mode && getMode (values[(size_t) mode]) == Mode::duck;
}

void Compressor::reset()
{
    reduction.reset();

    keyPeaks = {};
    keyPeakIndex = 0;
    keyPeakSamples = 0;
    keyPeakNow = 0.0f;
    keyPeakBefore = 0.0f;
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

    for (auto& filter : lowCuts)
        filter.reset();

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
    gainDelay.reset();
    isFirstBlock = true;
    keyShare = 0.0f;
    meanSquare = 0.0f;
    releaseStart = 0.0f;
    memory = 0.0f;
    lastLevel = -1.0f;
    lastWanted = 0.0f;
    colourAmount = 0.0f;
}

void Compressor::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    silenceWhatIsNotSound (block);

    if (getMode (block.parameters[mode]) == Mode::duck)
        duck (block);
    else
        compress (block);

    isFresh = false;
}

void Compressor::compress (const ProcessContext& block)
{
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);
    const auto type = getCharacter (values[character]);

    // What the detector hears: the sound itself as it comes in, after the input gain; not a sidechain, which only Duck hears.
    // Through the key's low cut alone, as a bus compressor's sidechain high-pass keeps the bass from pumping it: the high cut
    // is Duck's, for a kick's body heard from a whole kit, and its page shows it there alone.
    const float* const* heard = block.channels;
    const auto numHeard = numChannels;

    const auto targetMakeup = values[makeup];
    const auto targetInput = values[inputGain];
    const auto hearsAverage = type == Character::vca || type == Character::opto || type == Character::tube;
    const auto lowCut = dsp::makeFilter (dsp::FilterShape::highPass, values[keyLowCut], sampleRate, 0.70710678);
    const auto keyWanted = isPlayingKey() ? 1.0f : 0.0f;
    const auto colourOf = colourShares[(size_t) type];
    const auto even = evenShares[(size_t) type];
    const auto dcBlocker = dsp::makeFilter (dsp::FilterShape::highPass, dcBlockerFrequency, 2.0 * sampleRate, 0.70710678);
    gains.update (getCurve (values));
    reduction.setTimes (values[attack] * 0.001f, values[release] * 0.001f);

    // Opto's slow stage, after its first half: slower still the longer it has held lately.
    const auto slowRelease = values[release] * 0.001f * (opticalSlowdown + opticalMemorySlowdown * memory);
    opticalCoefficient = (float) std::exp (-1.0 / ((double) slowRelease * sampleRate));

    // How far the drive's curve is in: the knob's share over its first tenth, or FET's and Tube's colour's over their first
    // 3 dB of reduction, the more of the two; the knob's glides, and the colour's follows how far it turns down, smoothly,
    // sample by sample, so that it plays the same however its blocks are cut.
    const auto knobShareWanted = std::clamp (values[drive] / curveFadeDrive, 0.0f, 1.0f);

    if (isFirstBlock)
    {
        makeupDecibels = targetMakeup;
        inputDecibels = targetInput;
        driveAmount = values[drive];
        knobShare = knobShareWanted;
        curveShare = knobShare;
        isFirstBlock = false;
    }

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

    // The copy of what came in for the mix goes through filters like the drive's with the sound, while the drive plays, so that
    // they are ready whenever the mix leaves 100 %; where the sound then takes Duck's latency, the copy goes on through a delay
    // for the rest of it as the sound does.
    const auto rest = latency - driveLatency;
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

        // What it hears, through its low cut.
        auto peak = 0.0f, squares = 0.0f;
        std::array<float, 2> key {};

        for (int channel = 0; channel < numHeard; ++channel)
        {
            const auto x = lowCuts[(size_t) channel].process (heard[channel][i] * lastInput, lowCut);
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
        colourAmount += (std::max (0.0f, amount) - colourAmount) * makeupSmoothing;

        if (const auto gainDecibels = makeupDecibels - amount; ! juce::exactlyEqual (gainDecibels, lastGainDecibels))
        {
            lastGainDecibels = gainDecibels;
            lastGain = dsp::decibelsToGain (gainDecibels);
        }

        // The sound, turned down and driven into its curve (the makeup after it); or what it hears, to listen to, faded in and
        // out.
        keyShare += std::clamp (keyWanted - keyShare, -keyStep, keyStep);

        // Duck's gain, which it plays late, is none for what goes through now: should automation have it duck next, it turns
        // what is still on its way no further down.
        gainDelay.push (1.0f);

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

        if (drivesRun && ! juce::exactlyEqual (makeupDecibels, lastMakeupDecibels))
        {
            lastMakeupDecibels = makeupDecibels;
            makeupGain = dsp::decibelsToGain (makeupDecibels);
        }

        for (int channel = 0; channel < numChannels; ++channel)
        {
            // The sound, turned down, as late as the drive plays it.
            const auto c = (size_t) channel;
            const auto turnedDown = block.channels[channel][i] * lastInput * lastGain;
            undriven[c].push (turnedDown);
            auto sound = undriven[c].read (driveLatency);

            if (drivesRun)
            {
                // Without the makeup, into the curve and back out at the level it came in at, as far as the drive has faded
                // the curve in; then the makeup. While nothing drives it, the straight line alone, through the same filters.
                const auto plain = turnedDown / makeupGain;
                auto y = drives[c].play (plain, driveGain, curve, curveShare, dcBlocker, playingEven > 0.0f);

                if (changesCurve)
                    y += nextBlend * (nextDrives[c].play (plain, driveGain, nextCurve, curveShare, dcBlocker, nextEven > 0.0f) - y);

                sound += driveBlend * (y * makeupGain - sound);
            }

            // What it hears, as late, to listen to.
            keyDelays[c].push (key[(size_t) std::min (channel, numHeard - 1)]);
            auto out = sound + keyShare * (keyDelays[c].read (driveLatency) - sound);

            // As late as its latency says, when that is Duck's (while automation has it compress).
            if (rest > 0)
            {
                auto& delay = soundDelays[c];
                delay.push (out);
                out = delay.read (rest);
            }

            block.channels[channel][i] = out;

            // The copy of what came in, as late, the same way; and turned as the drive's filters turn the sound.
            if (block.dry != nullptr)
            {
                auto& came = block.dry[channel][i];
                dryDelays[c].push (came);
                auto late = dryDelays[c].read (driveLatency);

                if (turnsDry)
                    late += driveBlend * (dryDrives[c].process (came, dsp::StraightCurve {}) - late);

                if (rest > 0)
                {
                    dryRests[c].push (late);
                    late = dryRests[c].read (rest);
                }

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
    const auto ahead = std::clamp ((int) std::ceil (attackSamples), 0, ducksAsPrepared ? latency : 0);
    const auto gainLate = latency - ahead;
    const auto rest = latency - driveLatency; // what the sound is late by after the drive's own delay
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

        keyPeakNow = std::max (keyPeakNow, peak);

        if (++keyPeakSamples >= samplesPerPeak)
        {
            keyPeaks[(size_t) keyPeakIndex] = keyPeakNow;
            keyPeakIndex = (keyPeakIndex + 1) % (int) keyPeaks.size();
            keyPeakSamples = 0;
            keyPeakNow = 0.0f;
            keyPeakBefore = *std::max_element (keyPeaks.begin(), keyPeaks.end());
        }

        if (const auto level = std::max (keyPeakBefore, keyPeakNow); ! juce::exactlyEqual (level, lastKeyPeak))
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
            gainDelay.push (duckGain);
            gain = gainDelay.read (gainLate);
        }

        for (int channel = 0; channel < numChannels; ++channel)
        {
            // The sound through the delays Compress sends it through, the drive's and then one for the rest of the latency, so
            // that a change of mode plays on what is on its way through them: nothing lost, nothing played again.
            const auto c = (size_t) channel;
            undriven[c].push (block.channels[channel][i]);
            auto sound = undriven[c].read (driveLatency);

            if (rest > 0)
            {
                soundDelays[c].push (sound);
                sound = soundDelays[c].read (rest);
            }

            keyDelays[c].push (key[(size_t) std::min (channel, numHeard - 1)]);
            const auto keyHeard = keyDelays[c].read (latency);
            const auto turnedDown = sound * gain;
            block.channels[channel][i] = turnedDown + keyShare * (keyHeard - turnedDown);

            // The copy of what came in for the mix, as late as the sound and the same way (as Compress sends it), so that a
            // change of mode plays on what is on its way.
            if (block.dry != nullptr)
            {
                auto& came = block.dry[channel][i];
                dryDelays[c].push (came);
                auto late = dryDelays[c].read (driveLatency);

                if (rest > 0)
                {
                    dryRests[c].push (late);
                    late = dryRests[c].read (rest);
                }

                came = late;
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
} // namespace phazite::devices
