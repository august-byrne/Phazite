// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Stereo.h"
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
        { .id = "balance", .name = "Balance", .minimum = -1.0f, .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::pan },
        { .id = "mono", .name = "Mono", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::toggle },
        { .id = "swap", .name = "Swap Sides", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::toggle },
        { .id = "bassMono", .name = "Bass Mono", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::toggle },
        { .id = "bassMonoFrequency",
          .name = "Bass Mono Frequency",
          .minimum = 40.0f,
          .maximum = 500.0f,
          .defaultValue = 120.0f,
          .unit = Unit::hertz,
          .scale = Scale::logarithmic },
        { .id = "invertLeft", .name = "Invert Left", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::toggle },
        { .id = "invertRight", .name = "Invert Right", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::toggle },
        { .id = "dcFilter", .name = "DC Filter", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::toggle },
        { .id = "gain", .name = "Gain", .minimum = -36.0f, .maximum = 36.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
    };

    constexpr double smoothingSeconds = 0.02;  // how long a change takes, about
    constexpr double averageSeconds = 0.5;     // the correlation's averaging, as hardware meters take about 600 ms
    constexpr float quietest = 1.0e-6f;        // -60 dB, squared: below it, no correlation is shown
    constexpr std::uint32_t velvetSeed = 2017; // the velvet noise's pulses, the same every time
    constexpr double landing = 1.0e-6;         // how near a glide comes before it lands on what it goes to
    constexpr float quarterTurn = 1.57079633f; // of the balance's cosine, at its end

    // A step of a glide that lands exactly on what it goes to, so that a switch that is on swaps or plays mono exactly,
    // and a balance turned back to the centre leaves the sound as it was. In doubles: a float a step at a time from 0 to 1
    // stops short, some 3e-5 under 1, where a step is less than half its last digit.
    void glide (double& value, float target, float smoothing) noexcept
    {
        value += ((double) target - value) * smoothing;

        if (std::abs ((double) target - value) < landing)
            value = target;
    }

    // What the balance leaves of the side it is turned away from: the side turned toward stays as it is (1, exactly, at the
    // centre), so that nothing is ever louder.
    float getTurnedAwayGain (float turnedAway) noexcept
    {
        return turnedAway > 0.0f ? std::max (0.0f, std::cos (quarterTurn * std::min (1.0f, turnedAway))) : 1.0f;
    }
} // namespace

const DeviceInfo Stereo::info { .typeID = typeID, .name = "Stereo", .kind = DeviceKind::effect, .parameters = parameters };

float Stereo::getBalanceGain (float turned, int side) noexcept
{
    // Turned right (above 0), the left fades; turned left, the right.
    return getTurnedAwayGain (side == 0 ? turned : -turned);
}

float Stereo::Correlation::get() const noexcept
{
    if (0.5 * (left + right) < quietest)
        return noReading;

    const auto energy = std::sqrt (left * right);
    return energy > 0.0 ? (float) std::clamp (product / energy, -1.0, 1.0) : 0.0f;
}

void Stereo::prepare (double newSampleRate, int)
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

    for (auto& blocker : dcBlockers)
        blocker.prepare (sampleRate);

    shownFrequency = 0.0f;
    reset();
}

void Stereo::reset()
{
    sideSplit.reset();
    middlePhase.reset();
    for (auto& filter : spreadHighPass)
        filter.reset();

    middleLine.reset();
    for (auto& blocker : dcBlockers)
        blocker.reset();

    inputAverage = outputAverage = {};
    isFirstBlock = true;
}

void Stereo::setFrequency (float hertz) noexcept
{
    shownFrequency = hertz;
    sideSplit.setFrequency (hertz, sampleRate);
    middlePhase.setFrequency (hertz, sampleRate);
    highPass = dsp::makeFilter (dsp::FilterShape::highPass, hertz, sampleRate, 0.70710678);
}

void Stereo::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    silenceWhatIsNotSound (block);
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);

    if (numChannels <= 0)
        return;

    // Each side's polarity, first, so that what follows works on the sound put right; then the swap, mid and side, Mono and
    // the balance; the DC filter; the gain, last.
    const auto targetLeft = values[invertLeft] > 0.5f ? -1.0f : 1.0f;
    const auto targetRight = values[invertRight] > 0.5f ? -1.0f : 1.0f;
    const auto targetLevel = dsp::decibelsToGain (values[gain]);
    const auto targetSide = values[width], targetSpread = values[spread];
    const auto targetBass = values[bassMono] > 0.5f ? 1.0f : 0.0f;
    const auto targetSwap = values[swap] > 0.5f ? 1.0f : 0.0f;
    const auto targetMono = values[mono] > 0.5f ? 1.0f : 0.0f;
    const auto targetLeftBalance = getBalanceGain (values[balance], 0);
    const auto targetRightBalance = getBalanceGain (values[balance], 1);

    if (isFirstBlock)
    {
        leftGain = targetLeft;
        rightGain = targetRight;
        levelGain = targetLevel;
        sideGain = targetSide;
        spreadGain = targetSpread;
        bassAmount = targetBass;
        swapAmount = targetSwap;
        monoAmount = targetMono;
        leftBalance = targetLeftBalance;
        rightBalance = targetRightBalance;
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
        sideGain += (targetSide - sideGain) * smoothing;
        spreadGain += (targetSpread - spreadGain) * smoothing;
        bassAmount += (targetBass - bassAmount) * smoothing;
        glide (swapAmount, targetSwap, smoothing);
        glide (monoAmount, targetMono, smoothing);
        glide (leftBalance, targetLeftBalance, smoothing);
        glide (rightBalance, targetRightBalance, smoothing);

        // A mono channel: the left's polarity, then the DC filter and the gain, below.
        if (right == nullptr)
        {
            left[i] *= leftGain;
            continue;
        }

        auto l = left[i], r = right[i];
        inputAverage.add (l, r, averageWeight);
        l *= leftGain;
        r *= rightGain;

        // The sides swapped, crossfaded as the switch turns, so that it never clicks; exactly each other's once it is on.
        if (swapAmount > 0.0)
        {
            const auto swapped = (float) swapAmount, kept = (float) (1.0 - swapAmount);
            const auto swappedLeft = l * kept + r * swapped;
            r = r * kept + l * swapped;
            l = swappedLeft;
        }

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

        // Mono: what is left of the side turned down to nothing, both sides the middle.
        if (monoAmount > 0.0)
            side *= (float) (1.0 - monoAmount);

        // The balance: the side turned away from fades.
        left[i] = (middle + side) * (float) leftBalance;
        right[i] = (middle - side) * (float) rightBalance;
    }

    // The DC filter, a side each, only while it is on or fading out (dsp/DcBlocker.h): it turns the phase of the lows, so
    // that switched off, the sound passes as it was, bit for bit.
    const auto filtered = values[dcFilter] > 0.5f;

    for (size_t side = 0; side < (right != nullptr ? 2u : 1u); ++side)
        dcBlockers[side].process (side == 0 ? left : right, block.numSamples, filtered, false);

    // The gain, and how alike the sides are going out.
    for (int i = 0; i < block.numSamples; ++i)
    {
        levelGain += (targetLevel - levelGain) * smoothing;
        left[i] *= levelGain;

        if (right != nullptr)
        {
            right[i] *= levelGain;
            outputAverage.add (left[i], right[i], averageWeight);
        }
    }

    inputCorrelation.set (inputAverage.get());
    outputCorrelation.set (outputAverage.get());
}
} // namespace phazite::devices
