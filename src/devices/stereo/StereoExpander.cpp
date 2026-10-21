// SPDX-License-Identifier: AGPL-3.0-or-later
#include "StereoExpander.h"
#include "devices/dsp/Math.h"
#include "devices/dsp/Random.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
namespace
{
    // The IDs are saved in projects.
    constexpr ParameterInfo parameters[] = {
        { .id = "width", .name = "Width", .maximum = 2.0f, .defaultValue = 1.0f, .unit = Unit::percent },
        { .id = "spread", .name = "Spread", .defaultValue = 0.0f, .unit = Unit::percent },
        { .id = "bassMono", .name = "Bass Mono", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::toggle },
        { .id = "bassMonoFrequency",
          .name = "Bass Mono Frequency",
          .minimum = 40.0f,
          .maximum = 500.0f,
          .defaultValue = 120.0f,
          .unit = Unit::hertz,
          .midpoint = 140.0f },
        { .id = "invertLeft", .name = "Invert Left", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::toggle },
        { .id = "invertRight", .name = "Invert Right", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::toggle },
        { .id = "gain", .name = "Gain", .minimum = -36.0f, .maximum = 36.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
    };

    // In a mixer slot: how wide, and how much stereo it makes.
    constexpr std::string_view keyControls[] = { "width", "spread" };

    constexpr double smoothingSeconds = 0.02;  // how long a change takes, about
    constexpr double averageSeconds = 0.5;     // the correlation's averaging, as hardware meters take about 600 ms
    constexpr float quietest = 1.0e-6f;        // -60 dB, squared: below it, no correlation is shown
    constexpr std::uint32_t velvetSeed = 2017; // the velvet noise's pulses, the same every time
} // namespace

const DeviceInfo StereoExpander::info { .typeID = typeID,
                                        .name = "Stereo Expander",
                                        .kind = DeviceKind::effect,
                                        .parameters = parameters,
                                        .keyControls = keyControls };

float StereoExpander::Correlation::get() const noexcept
{
    if (0.5 * (left + right) < quietest)
        return noReading;

    const auto energy = std::sqrt (left * right);
    return energy > 0.0 ? (float) std::clamp (product / energy, -1.0, 1.0) : 0.0f;
}

void StereoExpander::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    smoothing = (float) (1.0 - std::exp (-1.0 / (smoothingSeconds * sampleRate)));
    averageWeight = 1.0 - std::exp (-1.0 / (averageSeconds * sampleRate));

    // The velvet noise: a pulse in each stretch of the 20 ms, at a place and of a sign chosen at random, each quieter than
    // the one before, all together as loud as the middle itself.
    const auto length = std::max (numTaps, (int) std::lround (decorrelatorSeconds * sampleRate));
    middleLine.prepare (length + 1);
    dsp::Random random (velvetSeed);
    auto energy = 0.0;

    for (int k = 0; k < numTaps; ++k)
    {
        const auto stretch = (double) length / numTaps;
        tapDelays[(size_t) k] = std::min (length, (int) (k * stretch + random.nextUnipolar() * stretch));
        tapGains[(size_t) k] = (random.nextUnipolar() < 0.5f ? -1.0f : 1.0f) * std::exp (-3.0f * (float) k / (float) numTaps);
        energy += (double) tapGains[(size_t) k] * tapGains[(size_t) k];
    }

    for (auto& tapGain : tapGains)
        tapGain = (float) (tapGain / std::sqrt (energy));

    shownFrequency = 0.0f;
    reset();
}

void StereoExpander::reset()
{
    sideSplit.reset();
    middlePhase.reset();
    for (auto& filter : spreadHighPass)
        filter.reset();

    middleLine.reset();
    inputAverage = outputAverage = {};
    isFirstBlock = true;
}

void StereoExpander::setFrequency (float hertz) noexcept
{
    shownFrequency = hertz;
    sideSplit.setFrequency (hertz, sampleRate);
    middlePhase.setFrequency (hertz, sampleRate);
    highPass = dsp::makeFilter (dsp::FilterShape::highPass, hertz, sampleRate, 0.70710678);
}

void StereoExpander::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);

    if (numChannels <= 0)
        return;

    // Each side's polarity, first, so that what follows works on the sound put right; the gain, last.
    const auto targetLeft = values[invertLeft] > 0.5f ? -1.0f : 1.0f;
    const auto targetRight = values[invertRight] > 0.5f ? -1.0f : 1.0f;
    const auto targetLevel = dsp::decibelsToGain (values[gain]);
    const auto targetSide = values[width], targetSpread = values[spread];
    const auto targetBass = values[bassMono] > 0.5f ? 1.0f : 0.0f;

    if (isFirstBlock)
    {
        leftGain = targetLeft;
        rightGain = targetRight;
        levelGain = targetLevel;
        sideGain = targetSide;
        spreadGain = targetSpread;
        bassAmount = targetBass;
        isFirstBlock = false;
    }

    // The bass mono's frequency, which the spread's high-pass shares, glides in octaves.
    if (std::abs (values[bassMonoFrequency] - shownFrequency) > 0.01f)
        setFrequency (shownFrequency <= 0.0f ? values[bassMonoFrequency]
                                             : shownFrequency * std::pow (values[bassMonoFrequency] / shownFrequency, 0.25f));

    auto* left = block.channels[0];
    auto* right = numChannels > 1 ? block.channels[1] : nullptr;

    for (int i = 0; i < block.numSamples; ++i)
    {
        leftGain += (targetLeft - leftGain) * smoothing;
        rightGain += (targetRight - rightGain) * smoothing;
        levelGain += (targetLevel - levelGain) * smoothing;
        sideGain += (targetSide - sideGain) * smoothing;
        spreadGain += (targetSpread - spreadGain) * smoothing;
        bassAmount += (targetBass - bassAmount) * smoothing;

        // A mono channel: the gain and the left's polarity only.
        if (right == nullptr)
        {
            left[i] *= leftGain * levelGain;
            continue;
        }

        auto l = left[i], r = right[i];
        inputAverage.add (l, r, averageWeight);
        l *= leftGain;
        r *= rightGain;

        // Through mid and side.
        auto middle = 0.5f * (l + r), side = 0.5f * (l - r);

        // Spread: the middle above the bass (as steeply as Bass Mono cuts it), smeared by the velvet noise, added to the side.
        middleLine.push (spreadHighPass[1].process (spreadHighPass[0].process (middle, highPass), highPass));

        if (spreadGain > 1.0e-6f)
        {
            auto smeared = 0.0f;

            for (int k = 0; k < numTaps; ++k)
                smeared += tapGains[(size_t) k] * middleLine.read (tapDelays[(size_t) k]);

            side += spreadGain * smeared;
        }

        side *= sideGain;

        // Bass mono: the side's bass taken away, and the middle kept in phase with what is left of the side. Both run all
        // the time, so that switching it crossfades.
        const auto highSide = sideSplit.process (side).high;
        const auto phasedMiddle = middlePhase.processAllPass (middle);
        side += bassAmount * (highSide - side);
        middle += bassAmount * (phasedMiddle - middle);

        l = (middle + side) * levelGain;
        r = (middle - side) * levelGain;
        left[i] = l;
        right[i] = r;
        outputAverage.add (l, r, averageWeight);
    }

    inputCorrelation.set (inputAverage.get());
    outputCorrelation.set (outputAverage.get());
}
} // namespace phazite::devices
