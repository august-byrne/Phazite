// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Compressor.h"
#include "devices/dsp/Math.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

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
          .midpoint = 100.0f },
        { .id = "keyHighCut",
          .name = "Sidechain High Cut",
          .minimum = 100.0f,
          .maximum = 20000.0f,
          .defaultValue = 20000.0f,
          .unit = Unit::hertz,
          .midpoint = 1500.0f },
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
    // its antiderivative, F (x) = ln cosh x + e (x - tanh x), written so that cosh never overflows.
    float drivenCurve (float x, float even) noexcept
    {
        const auto t = std::tanh (x);
        return t + even * t * t;
    }

    double drivenIntegral (double x, double even) noexcept
    {
        const auto a = std::abs (x);
        return a + std::log1p (std::exp (-2.0 * a)) - 0.69314718055994531 + even * (x - std::tanh (x));
    }
} // namespace

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
    latency = ducksAsPrepared ? (int) std::lround (duckLookaheadSeconds * sampleRate) : 0;

    for (auto* delays : { &soundDelays, &keyDelays })
        for (auto& delay : *delays)
            delay.prepare (std::max (1, latency));

    gainDelay.prepare (std::max (1, latency));
    reset();
}

int Compressor::getLatencySamplesFor (std::span<const float> values) const
{
    const auto ducks = values.size() > (size_t) mode && getMode (values[(size_t) mode]) == Mode::duck;
    return ducks ? (int) std::lround (duckLookaheadSeconds * sampleRate) : 0;
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

    for (auto& filter : dcBlockers)
        filter.reset();

    for (auto* delays : { &soundDelays, &keyDelays })
        for (auto& delay : *delays)
            delay.reset();

    gainDelay.reset();
    isFirstBlock = true;
    keyShare = 0.0f;
    meanSquare = 0.0f;
    releaseStart = 0.0f;
    memory = 0.0f;
    lastLevel = -1.0f;
    lastWanted = 0.0f;
    lastMost = 0.0f;
    wasDriven = false;
    lastDriven = {};
    lastIntegral = {};
}

void Compressor::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;

    if (getMode (block.parameters[mode]) == Mode::duck)
        duck (block);
    else
        compress (block);
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
    const auto targetDrive = std::min (1.0f, values[drive] + colourShares[(size_t) type] * std::min (1.0f, lastMost / colourReduction));
    const auto even = evenShares[(size_t) type];
    const auto dcBlocker = dsp::makeFilter (dsp::FilterShape::highPass, dcBlockerFrequency, sampleRate, 0.70710678);
    gains.update (getCurve (values));
    reduction.setTimes (values[attack] * 0.001f, values[release] * 0.001f);

    // Opto's slow stage, after its first half: slower still the longer it has held lately.
    const auto slowRelease = values[release] * 0.001f * (opticalSlowdown + opticalMemorySlowdown * memory);
    opticalCoefficient = (float) std::exp (-1.0 / ((double) slowRelease * sampleRate));

    if (isFirstBlock)
    {
        makeupDecibels = targetMakeup;
        inputDecibels = targetInput;
        driveAmount = targetDrive;
        isFirstBlock = false;
    }

    // The gain is worked out again only when it changes (docs/performance-audit.md, A5).
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
        driveAmount += (targetDrive - driveAmount) * makeupSmoothing;

        if (const auto gainDecibels = makeupDecibels - amount; ! juce::exactlyEqual (gainDecibels, lastGainDecibels))
        {
            lastGainDecibels = gainDecibels;
            lastGain = dsp::decibelsToGain (gainDecibels);
        }

        // The sound, turned down and driven into its curve (the makeup after it); or what it hears, to listen to, faded in and
        // out.
        keyShare += std::clamp (keyWanted - keyShare, -keyStep, keyStep);
        const auto driven = driveAmount > 1.0e-4f;

        if (driven && ! juce::exactlyEqual (driveAmount, lastDriveAmount))
        {
            lastDriveAmount = driveAmount;
            driveGain = dsp::decibelsToGain (driveAmount * mostDrive);
        }

        if (driven && ! juce::exactlyEqual (makeupDecibels, lastMakeupDecibels))
        {
            lastMakeupDecibels = makeupDecibels;
            makeupGain = dsp::decibelsToGain (makeupDecibels);
        }

        for (int channel = 0; channel < numChannels; ++channel)
        {
            auto turnedDown = block.channels[channel][i] * lastInput * lastGain;

            if (driven)
            {
                // Without the makeup, into the curve and back out at the level it came in at; then the makeup. Driven again
                // after a rest, it starts from where it is, so that it doesn't click.
                const auto c = (size_t) channel;
                const auto x = turnedDown / makeupGain * driveGain;
                const auto integral = (float) drivenIntegral ((double) x, (double) even);

                if (! wasDriven)
                {
                    lastDriven[c] = x;
                    lastIntegral[c] = integral;
                }

                auto y = std::abs (x - lastDriven[c]) > 1.0e-4f ? (integral - lastIntegral[c]) / (x - lastDriven[c])
                                                                : drivenCurve (0.5f * (x + lastDriven[c]), even);
                lastDriven[c] = x;
                lastIntegral[c] = integral;

                if (even > 0.0f)
                    y = dcBlockers[c].process (y, dcBlocker);

                turnedDown = y / driveGain * makeupGain;
            }

            auto out = turnedDown + keyShare * (key[(size_t) std::min (channel, numHeard - 1)] - turnedDown);

            // As late as its latency says, when it has one (Duck's, while automation has it compress).
            if (latency > 0)
            {
                auto& delay = soundDelays[(size_t) channel];
                delay.push (out);
                out = delay.read (latency);
            }

            block.channels[channel][i] = out;
        }

        wasDriven = driven;

        loudestPeak = std::max (loudestPeak, level);
        most = std::max (most, amount);
        least = std::min (least, amount);
    }

    levelMeter.raise (dsp::gainToDecibels (loudestPeak));
    reductionMeter.raise (most);
    boostMeter.raise (-least);
    gainReduction.push (most);
    lastMost = most;
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
    const auto ahead = std::clamp ((int) std::ceil (attackSamples), 0, latency);
    const auto gainLate = latency - ahead;
    const auto holdSamples =
        (int) std::min ((double) mostSinceHit, std::max (0.0, (double) values[hold] * 0.001 * sampleRate) + (double) ahead);
    const auto releaseStep = 1.0 / std::max (1.0, (double) values[release] * 0.001 * sampleRate);
    const auto curve = (double) juce::jlimit (-1.0f, 1.0f, values[releaseCurve]);
    const auto thresholdDecibels = values[Compressor::threshold];

    // Compress's drive starts again from where the sound is when it compresses again.
    wasDriven = false;
    auto loudestKey = 0.0f, most = 0.0f;

    for (int i = 0; i < block.numSamples; ++i)
    {
        // The key, through its cuts, and its loudest over the last few milliseconds.
        auto peak = 0.0f;
        std::array<float, 2> key {};

        for (int channel = 0; channel < numHeard; ++channel)
        {
            auto& cut = highCuts[(size_t) channel];
            auto x = lowCuts[(size_t) channel].process (heard[channel][i], lowCut);

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
            auto sound = block.channels[channel][i];
            auto keyHeard = key[(size_t) std::min (channel, numHeard - 1)];

            if (latency > 0)
            {
                auto& soundDelay = soundDelays[(size_t) channel];
                auto& keyDelay = keyDelays[(size_t) channel];
                soundDelay.push (sound);
                keyDelay.push (keyHeard);
                sound = soundDelay.read (latency);
                keyHeard = keyDelay.read (latency);
            }

            const auto turnedDown = sound * gain;
            block.channels[channel][i] = turnedDown + keyShare * (keyHeard - turnedDown);
        }

        loudestKey = std::max (loudestKey, lastKeyPeak);
        most = std::max (most, ducked);
    }

    levelMeter.raise (dsp::gainToDecibels (loudestKey));
    reductionMeter.raise (most);
    gainReduction.push (most);
    lastMost = 0.0f; // Compress's colour, which nothing turned down for
}
} // namespace phazite::devices
