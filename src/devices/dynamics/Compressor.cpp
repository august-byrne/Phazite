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
    constexpr std::string_view characters[] = { "Clean", "VCA", "FET", "Opto", "Tube" };

    // The IDs are saved in projects.
    constexpr ParameterInfo parameters[] = {
        { .id = "character",
          .name = "Character",
          .maximum = (float) std::size (characters) - 1.0f,
          .defaultValue = 0.0f,
          .unit = Unit::choice,
          .choices = characters },
        { .id = "inputGain", .name = "Input Gain", .minimum = -24.0f, .maximum = 24.0f, .unit = Unit::decibels },
        { .id = "threshold", .name = "Threshold", .minimum = -60.0f, .maximum = 0.0f, .defaultValue = -18.0f, .unit = Unit::decibels },
        { .id = "ratio", .name = "Ratio", .minimum = 1.0f, .maximum = 20.0f, .defaultValue = 4.0f, .unit = Unit::ratio, .midpoint = 4.0f },
        { .id = "attack",
          .name = "Attack",
          .minimum = 0.1f,
          .maximum = 200.0f,
          .defaultValue = 10.0f,
          .unit = Unit::milliseconds,
          .midpoint = 10.0f },
        { .id = "release",
          .name = "Release",
          .minimum = 5.0f,
          .maximum = 2000.0f,
          .defaultValue = 120.0f,
          .unit = Unit::milliseconds,
          .midpoint = 100.0f },
        { .id = "knee", .name = "Knee", .minimum = 0.0f, .maximum = 24.0f, .defaultValue = 6.0f, .unit = Unit::decibels },
        { .id = "makeup", .name = "Makeup", .minimum = -12.0f, .maximum = 24.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
        { .id = "drive", .name = "Drive", .defaultValue = 0.0f, .unit = Unit::percent },
        { .id = "keyLowCut",
          .name = "Key Low Cut",
          .minimum = 20.0f,
          .maximum = 500.0f,
          .defaultValue = 20.0f,
          .unit = Unit::hertz,
          .midpoint = 100.0f },
        { .id = "keyHighCut",
          .name = "Key High Cut",
          .minimum = 100.0f,
          .maximum = 20000.0f,
          .defaultValue = 20000.0f,
          .unit = Unit::hertz,
          .midpoint = 1500.0f },
        { .id = mixParameterID, .name = "Mix", .defaultValue = 1.0f, .unit = Unit::percent },
    };

    static_assert (std::size (parameters) == (size_t) Compressor::mix + 1);
    static_assert (std::size (characters) == (size_t) Compressor::Character::tube + 1);

    constexpr double makeupSeconds = 0.02;   // how long a change of the input or makeup gain, or of the drive, takes, about
    constexpr float highCutOff = 19999.0f;   // the key's high cut at its top: off
    constexpr double keyFadeSeconds = 0.005; // between the key and the sound, as Listen turns on or off

    // The characters' ears and their timing.
    constexpr double rmsSeconds = 0.01;            // the average a VCA, Opto or Tube hears, over about this long
    constexpr float tubeRange = 15.0f;             // decibels over the threshold where a tube's ratio has grown to the one set
    constexpr float opticalKnee = 12.0f;           // an Opto's knee is at least this wide
    constexpr float opticalSlowdown = 6.0f;        // after its first half, an Opto lets go this many times slower...
    constexpr float opticalMemorySlowdown = 12.0f; // ...and up to this many more times, after holding for a while
    constexpr double memoryRiseSeconds = 2.0, memoryFallSeconds = 6.0; // how long holding takes to count, and to be forgotten
    constexpr float memoryHold = 3.0f;                                 // decibels of reduction that count as holding

    // Drive: up to this many decibels into the curve, and how much of the even harmonics' curve each character adds to the
    // odd harmonics' (none for Clean and FET, a little for VCA, more for Opto and the most for Tube).
    constexpr float mostDrive = 18.0f;
    constexpr float evenShares[] = { 0.0f, 0.05f, 0.0f, 0.15f, 0.3f };
    constexpr double dcBlockerFrequency = 10.0; // under what the even harmonics leave

    Compressor::Character getCharacter (float value) noexcept
    {
        return (Compressor::Character) juce::jlimit (0, (int) std::size (characters) - 1, (int) std::lround (value));
    }

    // How many decibels a level goes down by: (1 - 1 / ratio) of the way it goes over the threshold, easing in across
    // the knee, which is centred on the threshold (Giannoulis, Massberg and Reiss, "Digital Dynamic Range Compressor
    // Design: A Tutorial and Analysis", 2012).
    float computeReduction (float level, float threshold, float slope, float knee) noexcept
    {
        const auto over = level - threshold;

        if (2.0f * over <= -knee)
            return 0.0f;

        if (2.0f * over < knee) // within the knee, which is wider than 0 here
        {
            const auto into = over + 0.5f * knee;
            return slope * into * into / (2.0f * knee);
        }

        return slope * over;
    }

    // A tube's (variable-mu) curve: no reduction at the threshold, its ratio growing over the next tubeRange decibels to
    // the one set, as the tube's bias shifts with the level (Moore, 2012: a variable-mu compressor has no ratio of its own);
    // the slope grows in a straight line, so the reduction grows as its square, then as the slope set.
    float computeTubeReduction (float level, float threshold, float slope) noexcept
    {
        const auto over = level - threshold;

        if (over <= 0.0f)
            return 0.0f;

        if (over < tubeRange)
            return slope * over * over / (2.0f * tubeRange);

        return slope * (over - 0.5f * tubeRange);
    }

    float getSlope (float ratio) noexcept
    {
        return 1.0f - 1.0f / std::max (ratio, 1.0f);
    }

    float getReductionFor (Compressor::Character character, float level, float threshold, float slope, float knee) noexcept
    {
        switch (character)
        {
            case Compressor::Character::tube:
                return computeTubeReduction (level, threshold, slope);
            case Compressor::Character::opto:
                return computeReduction (level, threshold, slope, std::max (knee, opticalKnee));
            case Compressor::Character::clean:
            case Compressor::Character::vca:
            case Compressor::Character::fet:
            default:
                return computeReduction (level, threshold, slope, knee);
        }
    }

    // Where the reduction starts, in decibels: under it, the level needs no decibels worked out.
    float getReductionStart (Compressor::Character character, float threshold, float knee) noexcept
    {
        switch (character)
        {
            case Compressor::Character::tube:
                return threshold;
            case Compressor::Character::opto:
                return threshold - 0.5f * std::max (knee, opticalKnee);
            case Compressor::Character::clean:
            case Compressor::Character::vca:
            case Compressor::Character::fet:
            default:
                return threshold - 0.5f * knee;
        }
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

float Compressor::getReduction (std::span<const float> values, float levelDecibels)
{
    return getReductionFor (getCharacter (values[character]), levelDecibels, values[threshold], getSlope (values[ratio]), values[knee]);
}

void Compressor::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    reduction.prepare (sampleRate);
    makeupSmoothing = (float) (1.0 - std::exp (-1.0 / (makeupSeconds * sampleRate)));
    keyStep = (float) std::min (1.0, 1.0 / (keyFadeSeconds * sampleRate));
    rmsSmoothing = (float) (1.0 - std::exp (-1.0 / (rmsSeconds * sampleRate)));
    memoryRise = (float) (1.0 - std::exp (-1.0 / (memoryRiseSeconds * sampleRate)));
    memoryFall = (float) (1.0 - std::exp (-1.0 / (memoryFallSeconds * sampleRate)));
    reset();
}

void Compressor::reset()
{
    reduction.reset();

    for (auto& filter : lowCuts)
        filter.reset();

    for (auto& channel : highCuts)
        for (auto& filter : channel)
            filter.reset();

    for (auto& filter : dcBlockers)
        filter.reset();

    isFirstBlock = true;
    keyShare = 0.0f;
    meanSquare = 0.0f;
    releaseStart = 0.0f;
    memory = 0.0f;
    lastReductionDecibels = 0.0f;
    lastReductionGain = 1.0f;
    wasDriven = false;
    lastDriven = {};
    lastIntegral = {};
}

void Compressor::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);
    const auto type = getCharacter (values[character]);

    // What the detector hears: the sidechain if one feeds it, else the sound itself; FET, Opto and Tube listen to what they put
    // out (feedback), Clean and VCA to what comes in.
    const float* const* heard = block.channels;
    auto numHeard = numChannels;

    if (block.sidechain != nullptr && block.numSidechainChannels > 0)
    {
        heard = block.sidechain;
        numHeard = std::min (block.numSidechainChannels, 2);
    }

    const auto thresholdDecibels = values[threshold], kneeDecibels = values[knee], targetMakeup = values[makeup];
    const auto targetInput = values[inputGain];
    const auto hearsItself = heard == block.channels; // the input gain drives the detector too, unless a sidechain feeds it
    const auto feedsBack = hearsItself && (type == Character::fet || type == Character::opto || type == Character::tube);
    const auto hearsAverage = type == Character::vca || type == Character::opto || type == Character::tube;
    const auto slope = getSlope (values[ratio]);
    const auto lowCut = dsp::makeFilter (dsp::FilterShape::highPass, values[keyLowCut], sampleRate, 0.70710678);
    const auto cutsHigh = values[keyHighCut] < highCutOff;
    const std::array highCut { dsp::makeFilter (dsp::FilterShape::lowPass, values[keyHighCut], sampleRate, dsp::getButterworthQ (4, 0)),
                               dsp::makeFilter (dsp::FilterShape::lowPass, values[keyHighCut], sampleRate, dsp::getButterworthQ (4, 1)) };
    const auto keyWanted = isPlayingKey() ? 1.0f : 0.0f;
    const auto targetDrive = values[drive];
    const auto even = evenShares[(size_t) type];
    const auto dcBlocker = dsp::makeFilter (dsp::FilterShape::highPass, dcBlockerFrequency, sampleRate, 0.70710678);
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

    // Under the reduction's start there is nothing to turn down, which the level tells without its decibels; and the gain is
    // worked out again only when it changes (docs/performance-audit.md, A5).
    const auto startGain = dsp::decibelsToGain (getReductionStart (type, thresholdDecibels, kneeDecibels));
    auto loudestPeak = 0.0f, most = 0.0f;

    for (int i = 0; i < block.numSamples; ++i)
    {
        // The input gain, gliding to where it is set.
        inputDecibels += (targetInput - inputDecibels) * makeupSmoothing;

        if (! juce::exactlyEqual (inputDecibels, lastInputDecibels))
        {
            lastInputDecibels = inputDecibels;
            lastInput = dsp::decibelsToGain (inputDecibels);
        }

        // The key, through its cuts: what comes in, or with feedback what went out, turned down as last worked out (before the
        // makeup, which comes after it, as on the hardware).
        const auto keyGain = hearsItself ? lastInput * (feedsBack ? lastReductionGain : 1.0f) : 1.0f;
        auto peak = 0.0f, squares = 0.0f;
        std::array<float, 2> key {};

        for (int channel = 0; channel < numHeard; ++channel)
        {
            auto& cut = highCuts[(size_t) channel];
            auto x = lowCuts[(size_t) channel].process (heard[channel][i] * keyGain, lowCut);

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

        const auto wanted =
            level <= startGain ? 0.0f : getReductionFor (type, dsp::gainToDecibels (level), thresholdDecibels, slope, kneeDecibels);
        const auto previous = reduction.getValue();
        auto amount = reduction.process (wanted);

        // Opto lets go at the release set for the first half of the way, as the LA-2A's cell does in its first 60 ms, then
        // slowly.
        if (type == Character::opto)
        {
            if (amount >= previous)
                releaseStart = amount; // gripping, or holding: where a release would start from
            else if (previous < 0.5f * releaseStart)
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

        if (feedsBack && ! juce::exactlyEqual (amount, lastReductionDecibels))
        {
            lastReductionDecibels = amount;
            lastReductionGain = dsp::decibelsToGain (-amount);
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

            block.channels[channel][i] = turnedDown + keyShare * (key[(size_t) std::min (channel, numHeard - 1)] - turnedDown);
        }

        wasDriven = driven;

        loudestPeak = std::max (loudestPeak, level);
        most = std::max (most, amount);
    }

    levelMeter.raise (dsp::gainToDecibels (loudestPeak));
    reductionMeter.raise (most);
    gainReduction.push (most);
}
} // namespace phazite::devices
