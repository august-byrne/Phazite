// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Exciter.h"
#include "devices/dsp/Math.h"

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
namespace
{
    // The IDs are saved in projects.
    constexpr ParameterInfo parameters[] = {
        { .id = "frequency",
          .name = "Frequency",
          .minimum = 1000.0f,
          .maximum = 12000.0f,
          .defaultValue = 5000.0f,
          .unit = Unit::hertz,
          .midpoint = 3500.0f },
        { .id = "drive", .name = "Drive", .minimum = 0.0f, .maximum = 36.0f, .defaultValue = 18.0f, .unit = Unit::decibels },
        { .id = "colour", .name = "Colour", .defaultValue = 0.5f, .unit = Unit::percent },
        { .id = "amount", .name = "Amount", .maximum = 4.0f, .defaultValue = 0.35f, .unit = Unit::percent, .midpoint = 0.5f },
        { .id = "highCut",
          .name = "High Cut",
          .minimum = 2000.0f,
          .maximum = 20000.0f,
          .defaultValue = 20000.0f,
          .unit = Unit::hertz,
          .midpoint = 8000.0f },
        { .id = mixParameterID, .name = "Mix", .defaultValue = 1.0f, .unit = Unit::percent },
    };

    constexpr double butterworthQ = 0.70710678;
    constexpr float highestCut = 20000.0f;      // High Cut's highest: off
    constexpr double reverseSeconds = 0.002;    // how long the filter after the curve is: the band's filters' ring at 1 kHz
    constexpr float leastTap = 1.0e-7f;         // under this, the end of the filters' ring is nothing
    constexpr double bandSeconds = 0.005;       // how long a move of one of the band's edges takes, about
    constexpr double gainSeconds = 0.02;        // how long a change of the drive, the colour or the amount takes, about
    constexpr double listenFadeSeconds = 0.005; // between the sound and what is added alone, as Listen turns on or off

    // How far Colour moves the curve's working point at 100 %: where softClip's third derivative is nothing, so that a
    // quiet sound's third harmonic is gone and its second leads.
    constexpr float mostBias = 0.7174389f;

    // softClip's slope at a point: ((9 - x^2) / (3 (3 + x^2)))^2 under 3, and flat from there.
    float getSlope (float x) noexcept
    {
        const auto x2 = x * x;

        if (x2 >= 9.0f)
            return 0.0f;

        const auto root = (9.0f - x2) / (3.0f * (3.0f + x2));
        return root * root;
    }

    bool hasHighCut (float highCut) noexcept
    {
        return highCut < highestCut - 0.5f;
    }

    // A filter of the band's a step nearer where it is going, its frequency gliding there (as the tangent the filter keeps
    // of it), so that a low note held in it doesn't jump as the band moves; true once it is there.
    bool glide (dsp::FilterCoefficients& now, const dsp::FilterCoefficients& target, float smoothing) noexcept
    {
        const auto g = now.g + (target.g - now.g) * smoothing;

        if (std::abs (target.g - g) <= 1.0e-5f * target.g)
        {
            now = target;
            return true;
        }

        now = target;
        now.g = g;
        now.a1 = 1.0f / (1.0f + g * (g + now.k));
        now.a2 = g * now.a1;
        now.a3 = g * now.a2;
        return false;
    }

    // The band's filters before the curve: two high-pass sections at the frequency, and two low-pass ones at High Cut
    // while it is on, as a gain at a frequency.
    double getFiltersGain (float frequency, float highCut, double at, double sampleRate)
    {
        const auto high =
            dsp::getMagnitude (dsp::makeFilter (dsp::FilterShape::highPass, frequency, sampleRate, butterworthQ), at, sampleRate);
        auto gain = high * high;

        if (hasHighCut (highCut))
        {
            const auto low =
                dsp::getMagnitude (dsp::makeFilter (dsp::FilterShape::lowPass, highCut, sampleRate, butterworthQ), at, sampleRate);
            gain *= low * low;
        }

        return gain;
    }
} // namespace

const DeviceInfo Exciter::info { .typeID = typeID, .name = "Exciter", .kind = DeviceKind::effect, .parameters = parameters };

float Exciter::shapeSample (float colourValue, float driven) noexcept
{
    const auto point = mostBias * std::clamp (colourValue, 0.0f, 1.0f);
    return (dsp::softClip (driven + point) - dsp::softClip (point)) / getSlope (point);
}

double Exciter::getBandGain (std::span<const float> values, double at, double rate)
{
    // Once through the filters before the curve, and once through the same played backwards after it.
    const auto once = getFiltersGain (values[frequency], values[highCut], at, rate);
    return once * once;
}

double Exciter::getLift (std::span<const float> values, double at, double rate)
{
    return 20.0 * std::log10 (1.0 + (double) values[amount] * getBandGain (values, at, rate));
}

void Exciter::prepare (double newSampleRate, int maximumBlockSize)
{
    sampleRate = newSampleRate;
    blockCapacity = std::max (1, maximumBlockSize);

    // The curve at four times the rate, or twice from 88.2 kHz on: the band is the top of the sound, so its harmonics pass
    // the Nyquist frequency sooner than a saturator's of the whole sound do. Through linear-phase half-band filters (the
    // saturator's turn the phase near the top, which is nothing to a sound that goes through whole; here, what the curve
    // takes off the band must come back in step with the band): they delay every frequency alike, a whole number of samples
    // at the curve's rate, as the curve's averaging and the filter that levels it again do (4). A few samples more there
    // make it a whole number at the sound's rate.
    const auto stages = sampleRate < 60000.0 ? 2 : 1;
    const auto factor = 1 << stages;
    curveRate = sampleRate * factor;
    oversampling = std::make_unique<juce::dsp::Oversampling<float>> (2, stages, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,
                                                                     true, false);
    oversampling->initProcessing ((size_t) blockCapacity);
    const auto late = (int) std::lround (oversampling->getLatencyInSamples() * (float) factor) + dsp::AntialiasedCurve::delay;
    padding = (factor - late % factor) % factor;
    curveLatency = (late + padding) / factor;

    reverseLength = std::max (2, (int) std::lround (reverseSeconds * sampleRate)) + 1;
    latency = curveLatency + reverseLength - 1;
    taps.assign ((size_t) reverseLength, 0.0f);
    band.assign ((size_t) (2 * blockCapacity), 0.0f);
    excited.assign ((size_t) (2 * blockCapacity), 0.0f);

    for (auto& channel : channels)
    {
        channel.band.prepare (curveLatency + 1);
        channel.sound.prepare (latency + 1);
        channel.history.assign ((size_t) (2 * reverseLength), 0.0f);
    }

    smoothing = (float) (1.0 - std::exp (-1.0 / (gainSeconds * curveRate)));
    amountSmoothing = (float) (1.0 - std::exp (-1.0 / (gainSeconds * sampleRate)));
    bandSmoothing = (float) (1.0 - std::exp (-1.0 / (bandSeconds * sampleRate)));
    cutStep = (float) (1.0 / (listenFadeSeconds * sampleRate));
    shareStep = (float) (1.0 / (listenFadeSeconds * sampleRate));
    lastFrequency = lastHighCut = -1.0f; // the first block works the band out
    reset();
}

void Exciter::reset()
{
    if (oversampling != nullptr)
        oversampling->reset();

    for (auto& channel : channels)
    {
        for (auto* filters : { &channel.highPasses, &channel.lowPasses })
            for (auto& filter : *filters)
                filter.reset();

        channel.curve.reset();
        channel.straight.reset();
        channel.padded.fill (0.0f);
        channel.band.reset();
        channel.sound.reset();
        std::fill (channel.history.begin(), channel.history.end(), 0.0f);
        channel.newest = 0;
    }

    rest.reset();

    addedShare = 0.0f;
    isFirstBlock = true;
}

// The band's filters, and the filter after the curve: the same filters' answer to an impulse, which the excited band is
// then measured against sample by sample, the oldest against the answer's start, which is those filters played backwards.
void Exciter::setBand (float newFrequency, float newHighCut)
{
    lastFrequency = newFrequency;
    lastHighCut = newHighCut;
    highPass = dsp::makeFilter (dsp::FilterShape::highPass, newFrequency, sampleRate, butterworthQ);
    cutsHighs = hasHighCut (newHighCut);

    if (cutsHighs)
        lowPass = dsp::makeFilter (dsp::FilterShape::lowPass, newHighCut, sampleRate, butterworthQ);

    std::array<dsp::Filter, 2> highs {}, lows {};

    for (int i = 0; i < reverseLength; ++i)
    {
        auto sample = highs[1].process (highs[0].process (i == 0 ? 1.0f : 0.0f, highPass), highPass);

        if (cutsHighs)
            sample = lows[1].process (lows[0].process (sample, lowPass), lowPass);

        taps[(size_t) i] = sample;
    }

    // Its last quarter fades out, so that where it is cut short (a band from 1 kHz still rings a little after 2 ms) nothing
    // breaks off.
    const auto fade = reverseLength / 4;

    for (int i = 0; i < fade; ++i)
        taps[(size_t) (reverseLength - fade + i)] *=
            0.5f * (1.0f + std::cos (juce::MathConstants<float>::pi * (float) (i + 1) / (float) fade));

    // Only as much of it as isn't nothing is played: a band that starts higher rings shorter.
    usedTaps = reverseLength;

    while (usedTaps > 1 && std::abs (taps[(size_t) usedTaps - 1]) < leastTap)
        --usedTaps;

    // And it adds up to nothing, as the filters' whole answer does, so that nothing steady the curve leaves (its working
    // point's level, a lopsided wave's middle) comes through.
    auto sum = 0.0;

    for (int i = 0; i < usedTaps; ++i)
        sum += taps[(size_t) i];

    const auto share = (float) (sum / usedTaps);

    for (int i = 0; i < usedTaps; ++i)
        taps[(size_t) i] -= share;
}

void Exciter::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);

    if (oversampling == nullptr || numChannels <= 0 || block.numSamples <= 0)
        return;

    if (! juce::exactlyEqual (values[frequency], lastFrequency) || ! juce::exactlyEqual (values[highCut], lastHighCut))
    {
        const auto cutBefore = cutsHighs;
        setBand (values[frequency], values[highCut]);

        // High Cut turned on: its filters start where it is, from nothing, and fade in.
        if (cutsHighs && ! cutBefore && cutShare <= 0.0f)
        {
            nowLow = lowPass;

            for (auto& channel : channels)
                for (auto& filter : channel.lowPasses)
                    filter.reset();
        }
    }

    const auto targetDrive = dsp::decibelsToGain (values[drive]);
    const auto targetBias = mostBias * std::clamp (values[colour], 0.0f, 1.0f);
    const auto targetAmount = std::max (0.0f, values[amount]);
    const auto wantedShare = isPlayingAdded() ? 1.0f : 0.0f;

    if (isFirstBlock)
    {
        driveGain = targetDrive;
        bias = targetBias;
        amountGain = targetAmount;
        nowHigh = highPass;
        nowLow = lowPass;
        cutShare = cutsHighs ? 1.0f : 0.0f;
        isFirstBlock = false;

        // The curves' levelling filters start on the working point's level, as if they had rested there, so that the first
        // samples aren't the way up to it.
        steepness = 1.0f / getSlope (bias);

        for (int i = 0; i < 2 * dsp::AntialiasedCurve::delay; ++i)
        {
            restLevel = rest.process (bias, dsp::SoftCurve {});

            for (int channel = 0; channel < numChannels; ++channel)
                channels[(size_t) channel].curve.process (bias, dsp::SoftCurve {});
        }

        restedFor = 2 * dsp::AntialiasedCurve::delay;
    }

    // In stretches as long as the block the device was prepared for, at most.
    for (int done = 0; done < block.numSamples;)
    {
        const auto length = std::min (blockCapacity, block.numSamples - done);
        float* bands[] = { band.data(), band.data() + blockCapacity };
        float* results[] = { excited.data(), excited.data() + blockCapacity };

        // The band: what is above the frequency (and below High Cut), its edges gliding to where they were moved.
        auto highMoves = ! juce::exactlyEqual (nowHigh.g, highPass.g), lowMoves = ! juce::exactlyEqual (nowLow.g, lowPass.g);
        const auto wantedCut = cutsHighs ? 1.0f : 0.0f;

        for (int i = 0; i < length; ++i)
        {
            if (highMoves)
                highMoves = ! glide (nowHigh, highPass, bandSmoothing);

            if (lowMoves)
                lowMoves = ! glide (nowLow, lowPass, bandSmoothing);

            cutShare += std::clamp (wantedCut - cutShare, -cutStep, cutStep);

            for (int channel = 0; channel < numChannels; ++channel)
            {
                auto& state = channels[(size_t) channel];
                const auto input = block.channels[channel][done + i];
                auto sample = state.highPasses[1].process (state.highPasses[0].process (input, nowHigh), nowHigh);

                if (cutShare > 0.0f)
                    sample += cutShare * (state.lowPasses[1].process (state.lowPasses[0].process (sample, nowLow), nowLow) - sample);

                bands[channel][i] = sample;
            }
        }

        // What the curve bends of it, at the curve's rate: the band driven into the curve around its working point, played by
        // its antiderivative, less the band as a straight line would leave it, played the same way, and the drive taken back
        // off. Quiet, that is nothing; driven, it is the harmonics, and how far the curve holds the band itself down.
        const juce::dsp::AudioBlock<const float> bandBlock (bands, (size_t) numChannels, (size_t) length);
        juce::dsp::AudioBlock<float> resultBlock (results, (size_t) numChannels, (size_t) length);
        auto fast = oversampling->processSamplesUp (bandBlock);
        auto reached = 0.0f;

        for (size_t i = 0; i < fast.getNumSamples(); ++i)
        {
            driveGain += (targetDrive - driveGain) * smoothing;

            // The working point glides to where Colour puts it. What the curve makes of the working point alone is taken off
            // what it makes of the band there: played the same way beside it while the point moves, so that it is as late
            // and as smooth, and nothing of the move itself comes out.
            if (! juce::exactlyEqual (bias, targetBias))
            {
                bias += (targetBias - bias) * smoothing;

                if (std::abs (targetBias - bias) < 1.0e-6f)
                    bias = targetBias;

                steepness = 1.0f / getSlope (bias);
                restedFor = 0;
            }

            if (restedFor < 2 * dsp::AntialiasedCurve::delay)
            {
                restLevel = rest.process (bias, dsp::SoftCurve {});
                ++restedFor;
            }

            const auto back = 1.0f / driveGain;
            ++paddedAt;

            for (size_t channel = 0; channel < fast.getNumChannels(); ++channel)
            {
                auto& state = channels[channel];
                auto* samples = fast.getChannelPointer (channel);
                const auto driven = samples[i] * driveGain;
                reached = std::max (reached, std::abs (driven));
                const auto bent = (state.curve.process (driven + bias, dsp::SoftCurve {}) - restLevel) * steepness;
                const auto made = (bent - state.straight.process (driven, dsp::StraightCurve {})) * back;

                // As many samples later as make its way a whole number of the sound's samples.
                state.padded[paddedAt & 3u] = made;
                samples[i] = state.padded[(paddedAt - (unsigned) padding) & 3u];
            }
        }

        oversampling->processSamplesDown (resultBlock);
        reach.raise (reached);

        // The excited band: the band itself, as late as what the curve bent of it comes back, and that; through the filter
        // after the curve; and added to the sound, as late as all of it.
        auto amountNow = amountGain, shareNow = addedShare;

        for (int channel = 0; channel < numChannels; ++channel)
        {
            auto& state = channels[(size_t) channel];
            auto* samples = block.channels[channel] + done;
            auto* result = results[channel];
            const auto* bandSamples = bands[channel];
            const auto* answer = taps.data();
            amountNow = amountGain;
            shareNow = addedShare;

            for (int i = 0; i < length; ++i)
            {
                state.band.push (bandSamples[i]);
                state.sound.push (samples[i]);
                const auto newSample = state.band.read (curveLatency) + result[i];

                state.newest = state.newest + 1 < reverseLength ? state.newest + 1 : 0;
                state.history[(size_t) state.newest] = state.history[(size_t) (state.newest + reverseLength)] = newSample;
                const auto* oldestFirst = state.history.data() + state.newest + 1;

                // Four sums side by side, so that they needn't wait for each other.
                float sums[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                int tap = 0;

                for (; tap + 4 <= usedTaps; tap += 4)
                {
                    sums[0] += oldestFirst[tap] * answer[tap];
                    sums[1] += oldestFirst[tap + 1] * answer[tap + 1];
                    sums[2] += oldestFirst[tap + 2] * answer[tap + 2];
                    sums[3] += oldestFirst[tap + 3] * answer[tap + 3];
                }

                for (; tap < usedTaps; ++tap)
                    sums[0] += oldestFirst[tap] * answer[tap];

                amountNow += (targetAmount - amountNow) * amountSmoothing;
                shareNow += std::clamp (wantedShare - shareNow, -shareStep, shareStep);
                const auto added = amountNow * ((sums[0] + sums[1]) + (sums[2] + sums[3]));
                result[i] = added;
                samples[i] = (1.0f - shareNow) * state.sound.read (latency) + added;
            }
        }

        amountGain = amountNow;
        addedShare = shareNow;
        addedTap.push (results[0], results[numChannels > 1 ? 1 : 0], length);
        done += length;
    }
}
} // namespace phazite::devices
