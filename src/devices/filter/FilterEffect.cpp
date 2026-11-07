// SPDX-License-Identifier: AGPL-3.0-or-later
#include "FilterEffect.h"
#include "devices/dsp/Math.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
namespace
{
    constexpr float lowestCutoff = 20.0f, highestCutoff = 20000.0f;

    // The IDs are saved in projects.
    constexpr ParameterInfo parameters[] = {
        { .id = "filterType",
          .name = "Type",
          .maximum = (float) std::size (dsp::filterModeNames) - 1.0f,
          .defaultValue = 0.0f,
          .unit = Unit::choice,
          .choices = dsp::filterModeNames },
        { .id = "cutoff",
          .name = "Cutoff",
          .minimum = lowestCutoff,
          .maximum = highestCutoff,
          .defaultValue = highestCutoff,
          .unit = Unit::hertz,
          .midpoint = 632.456f }, // the middle of the range, on a log scale
        { .id = "resonance", .name = "Resonance", .defaultValue = 0.1f, .unit = Unit::percent },
        { .id = "filterSlope",
          .name = "Slope",
          .maximum = 1.0f,
          .defaultValue = 1.0f,
          .unit = Unit::choice,
          .choices = dsp::filterSlopeNames },
        { .id = "drive", .name = "Drive", .maximum = 24.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
        { .id = mixParameterID, .name = "Mix", .defaultValue = 1.0f, .unit = Unit::percent },
    };

    constexpr double glideSeconds = 0.01; // how long the cutoff takes to follow a change, about
    constexpr double driveSeconds = 0.02;

    // The filter at a cutoff.
    dsp::MultimodeCoefficients makeCoefficients (std::span<const float> values, double cutoffHz, double sampleRate)
    {
        return dsp::makeMultimode ({ .mode = dsp::toFilterMode (values[FilterEffect::filterType]),
                                     .frequency = cutoffHz,
                                     .resonance = values[FilterEffect::resonance],
                                     .steep = dsp::isSteep (values[FilterEffect::filterSlope]),
                                     .q = dsp::getResonanceQ (values[FilterEffect::resonance]) },
                                   sampleRate);
    }

    float toOctaves (float hertz) noexcept
    {
        return std::log2 (std::max (hertz, lowestCutoff) / lowestCutoff);
    }

    double toHertz (float octavesAbove20) noexcept
    {
        return (double) lowestCutoff * std::pow (2.0, (double) octavesAbove20);
    }
} // namespace

const DeviceInfo FilterEffect::info { .typeID = typeID, .name = "Filter", .kind = DeviceKind::effect, .parameters = parameters };

double FilterEffect::getMagnitude (std::span<const float> values, double frequency, double sampleRate)
{
    return dsp::getMagnitude (makeCoefficients (values, values[cutoff], sampleRate), frequency, sampleRate);
}

void FilterEffect::prepare (double newSampleRate, int maximumBlockSize)
{
    sampleRate = newSampleRate;
    glide = (float) (1.0 - std::exp (-1.0 / (glideSeconds * sampleRate)));
    driveSmoothing = (float) (1.0 - std::exp (-1.0 / (driveSeconds * 2.0 * sampleRate)));

    // The drive at twice the rate, as the saturator's (Saturator.cpp): the half-band filters' latency, and the curve's 2.
    oversampling =
        std::make_unique<juce::dsp::Oversampling<float>> (2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);
    oversampling->initProcessing ((size_t) std::max (1, maximumBlockSize));
    latency = (int) std::lround (oversampling->getLatencyInSamples()) + dsp::AntialiasedCurve::delay / 2;

    for (size_t channel = 0; channel < 2; ++channel)
    {
        undriven[channel].prepare (latency);
        undrivenOut[channel].assign ((size_t) std::max (1, maximumBlockSize), 0.0f);
    }

    for (auto& filter : filters)
        filter.prepare (dsp::getLongestCombDelay (lowestCutoff, sampleRate));

    reset();
}

void FilterEffect::reset()
{
    if (oversampling != nullptr)
        oversampling->reset();

    for (auto& curve : curves)
        curve.reset();

    for (auto& filter : filters)
        filter.reset();

    for (auto& line : undriven)
        line.reset();

    isFirstBlock = true;
    wasDriven = false;
    driveBlend = 0.0f;
}

void FilterEffect::driveBlock (const ProcessContext& block, int numChannels, float targetDrive) noexcept
{
    // The sound as late as the drive makes it, for while the drive is off and to fade from as it comes on or goes off.
    const auto numSamples = std::min (block.numSamples, (int) undrivenOut[0].size());

    for (int channel = 0; channel < numChannels; ++channel)
    {
        auto& line = undriven[(size_t) channel];

        for (int i = 0; i < numSamples; ++i)
        {
            line.push (block.channels[channel][i]);
            undrivenOut[(size_t) channel][(size_t) i] = line.read (latency);
        }
    }

    const auto driveOn = targetDrive > 1.0001f;

    if (! driveOn && driveBlend <= 0.0f)
    {
        for (int channel = 0; channel < numChannels; ++channel)
            std::copy (undrivenOut[(size_t) channel].begin(), undrivenOut[(size_t) channel].begin() + numSamples, block.channels[channel]);

        driveGain = targetDrive;
        return;
    }

    // Coming on: from silence, faded in.
    if (driveBlend <= 0.0f)
    {
        oversampling->reset();

        for (auto& curve : curves)
            curve.reset();
    }

    // Twice as fast, the drive saturating what goes in, its quiet parts keeping their level (near 0 dB, straight).
    juce::dsp::AudioBlock<float> audio (block.channels, (size_t) numChannels, (size_t) numSamples);
    auto fast = oversampling->processSamplesUp (audio);

    for (size_t i = 0; i < fast.getNumSamples(); ++i)
    {
        driveGain += (targetDrive - driveGain) * driveSmoothing;
        const auto driven = driveGain > 1.0001f;

        if (driven != wasDriven)
        {
            for (auto& curve : curves)
                curve.restartAveraging();

            wasDriven = driven;
        }

        for (size_t channel = 0; channel < fast.getNumChannels(); ++channel)
        {
            auto& sample = fast.getChannelPointer (channel)[i];
            sample = driven ? curves[channel].process (sample * driveGain, dsp::SoftCurve {}) / driveGain
                            : curves[channel].process (sample, dsp::StraightCurve {});
        }
    }

    oversampling->processSamplesDown (audio);

    // Faded over 64 samples from the plain delay to the drive, or back.
    constexpr auto blendStep = 1.0f / 64.0f;
    const auto blendTarget = driveOn ? 1.0f : 0.0f;
    auto blend = driveBlend;

    for (int i = 0; i < numSamples; ++i)
    {
        blend += std::clamp (blendTarget - blend, -blendStep, blendStep);

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const auto plain = undrivenOut[(size_t) channel][(size_t) i];
            block.channels[channel][i] = plain + blend * (block.channels[channel][i] - plain);
        }
    }

    driveBlend = blend;
}

double FilterEffect::getTailSeconds() const
{
    return (double) tailSeconds.load (std::memory_order_relaxed);
}

void FilterEffect::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);
    const auto targetOctaves = toOctaves (values[cutoff]);
    const auto targetDrive = dsp::decibelsToGain (values[drive]);

    if (isFirstBlock)
    {
        octaves = targetOctaves;
        driveGain = targetDrive;
        isFirstBlock = false;
    }

    if (oversampling == nullptr || numChannels <= 0 || block.numSamples <= 0)
        return;

    driveBlock (block, numChannels, targetDrive);
    auto coefficients = makeCoefficients (values, toHertz (octaves), sampleRate);

    for (int start = 0; start < block.numSamples; start += coefficientStep)
    {
        const auto end = std::min (block.numSamples, start + coefficientStep);

        // The cutoff glides in octaves, so a sweep sounds even all the way; new coefficients every few samples, a comb's
        // delay gliding between them too.
        if (std::abs (targetOctaves - octaves) > 1.0e-4f)
        {
            octaves += (targetOctaves - octaves) * (1.0f - std::pow (1.0f - glide, (float) (end - start)));
            coefficients = makeCoefficients (values, toHertz (octaves), sampleRate);
        }

        for (int channel = 0; channel < numChannels; ++channel)
            filters[(size_t) channel].set (coefficients, end - start);

        for (int i = start; i < end; ++i)
            for (int channel = 0; channel < numChannels; ++channel)
                block.channels[channel][i] = filters[(size_t) channel].process (block.channels[channel][i]);
    }

    // How long a comb rings once its input stops: at the lower of where its cutoff is and where it is gliding to, the
    // longer. The other types fall silent within a few milliseconds.
    const auto lowest = makeCoefficients (values, std::min (toHertz (octaves), (double) values[cutoff]), sampleRate);
    const auto comb = lowest.mode == dsp::FilterMode::combPlus || lowest.mode == dsp::FilterMode::combMinus;
    tailSeconds.store (comb ? (float) dsp::getRingSeconds (lowest.comb, sampleRate) : 0.0f, std::memory_order_relaxed);
}
} // namespace phazite::devices
