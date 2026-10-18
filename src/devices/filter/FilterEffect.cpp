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
    // In the order of FilterEffect::Type, saved in projects by number.
    constexpr std::string_view types[] = { "Low Pass", "High Pass", "Band Pass", "Notch", "Comb +", "Comb -" };
    constexpr std::string_view slopes[] = { "12 dB", "24 dB" };

    constexpr float lowestCutoff = 20.0f, highestCutoff = 20000.0f;

    // The IDs are saved in projects: never change them.
    constexpr ParameterInfo parameters[] = {
        { .id = "type",
          .name = "Type",
          .maximum = (float) std::size (types) - 1.0f,
          .defaultValue = 0.0f,
          .unit = Unit::choice,
          .choices = types },
        { .id = "cutoff",
          .name = "Cutoff",
          .minimum = lowestCutoff,
          .maximum = highestCutoff,
          .defaultValue = highestCutoff,
          .unit = Unit::hertz,
          .midpoint = 632.456f }, // the middle of the range, on a log scale
        { .id = "resonance", .name = "Resonance", .defaultValue = 0.1f, .unit = Unit::percent },
        { .id = "slope", .name = "Slope", .maximum = 1.0f, .defaultValue = 1.0f, .unit = Unit::choice, .choices = slopes },
        { .id = "drive", .name = "Drive", .maximum = 24.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
        { .id = mixParameterID, .name = "Mix", .defaultValue = 1.0f, .unit = Unit::percent },
    };

    // In a mixer slot: the sweep.
    constexpr std::string_view keyControls[] = { "cutoff", "resonance" };

    constexpr double glideSeconds = 0.01; // how long the cutoff takes to follow a change, about
    constexpr double driveSeconds = 0.02;

    struct Sections
    {
        std::array<dsp::FilterCoefficients, 2> coefficients;
        int count = 1;
    };

    FilterEffect::Type getType (std::span<const float> values)
    {
        return (FilterEffect::Type) juce::jlimit (0, (int) std::size (types) - 1, (int) std::lround (values[FilterEffect::type]));
    }

    bool isComb (FilterEffect::Type kind)
    {
        return kind == FilterEffect::Type::combPlus || kind == FilterEffect::Type::combMinus;
    }

    dsp::CombCoefficients makeComb (std::span<const float> values, double cutoffHz, double sampleRate)
    {
        return dsp::makeComb (getType (values) == FilterEffect::Type::combMinus, cutoffHz, sampleRate, values[FilterEffect::resonance]);
    }

    // The filter's sections: one for 12 dB per octave; two for 24, the second carrying the resonance (as a fourth-order
    // Butterworth's sections do, -3 dB at the cutoff without it).
    Sections makeSections (std::span<const float> values, double cutoffHz, double sampleRate)
    {
        const auto kind = getType (values);
        const auto steep = values[FilterEffect::slope] > 0.5f;
        const auto amount = (double) juce::jlimit (0.0f, 1.0f, values[FilterEffect::resonance]);

        auto shape = dsp::FilterShape::lowPass;

        if (kind == FilterEffect::Type::highPass)
            shape = dsp::FilterShape::highPass;
        else if (kind == FilterEffect::Type::bandPass)
            shape = dsp::FilterShape::bandPass;
        else if (kind == FilterEffect::Type::notch)
            shape = dsp::FilterShape::notch;

        Sections sections;

        if (! steep)
        {
            sections.coefficients[0] = dsp::makeFilter (shape, cutoffHz, sampleRate, 0.70710678 * std::pow (2.0, amount * 4.5));
            return sections;
        }

        // Band-pass and notch get two alike, narrower and deeper; low-pass and high-pass a Butterworth pair.
        const auto plain = shape == dsp::FilterShape::bandPass || shape == dsp::FilterShape::notch;
        const auto firstQ = plain ? 0.70710678 * std::pow (2.0, amount * 4.5) : dsp::getButterworthQ (4, 0);
        const auto secondQ = plain ? firstQ : dsp::getButterworthQ (4, 1) * std::pow (2.0, amount * 4.0);
        sections.coefficients[0] = dsp::makeFilter (shape, cutoffHz, sampleRate, firstQ);
        sections.coefficients[1] = dsp::makeFilter (shape, cutoffHz, sampleRate, secondQ);
        sections.count = 2;
        return sections;
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

const DeviceInfo FilterEffect::info { .typeID = typeID,
                                      .name = "Filter",
                                      .kind = DeviceKind::effect,
                                      .parameters = parameters,
                                      .keyControls = keyControls };

double FilterEffect::getMagnitude (std::span<const float> values, double frequency, double sampleRate)
{
    if (isComb (getType (values)))
        return dsp::getMagnitude (makeComb (values, values[cutoff], sampleRate), frequency, sampleRate);

    const auto sections = makeSections (values, values[cutoff], sampleRate);
    auto magnitude = 1.0;

    for (int i = 0; i < sections.count; ++i)
        magnitude *= dsp::getMagnitude (sections.coefficients[(size_t) i], frequency, sampleRate);

    return magnitude;
}

void FilterEffect::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    glide = (float) (1.0 - std::exp (-1.0 / (glideSeconds * sampleRate)));
    driveSmoothing = (float) (1.0 - std::exp (-1.0 / (driveSeconds * sampleRate)));

    for (auto& comb : combs)
        comb.prepare (dsp::getLongestCombDelay (lowestCutoff, sampleRate));

    reset();
}

void FilterEffect::reset()
{
    for (auto& section : stages)
        for (auto& filter : section)
            filter.reset();

    for (auto& comb : combs)
        comb.reset();

    isFirstBlock = true;
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

    // A comb chosen again starts silent, rather than with what it held when it was last played.
    const auto comb = isComb (getType (values));

    if (comb && ! wasComb)
        for (auto& each : combs)
            each.reset();

    wasComb = comb;

    auto sections = makeSections (values, toHertz (octaves), sampleRate);
    auto combCoefficients = makeComb (values, toHertz (octaves), sampleRate);

    for (int start = 0; start < block.numSamples; start += coefficientStep)
    {
        const auto end = std::min (block.numSamples, start + coefficientStep);

        // The cutoff glides in octaves, so a sweep sounds even all the way; new coefficients every few samples.
        if (std::abs (targetOctaves - octaves) > 1.0e-4f)
        {
            octaves += (targetOctaves - octaves) * (1.0f - std::pow (1.0f - glide, (float) (end - start)));

            if (comb)
                combCoefficients = makeComb (values, toHertz (octaves), sampleRate);
            else
                sections = makeSections (values, toHertz (octaves), sampleRate);
        }

        // The comb's delay glides between them too.
        if (comb)
            for (int channel = 0; channel < numChannels; ++channel)
                combs[(size_t) channel].setTarget (combCoefficients, end - start);

        for (int i = start; i < end; ++i)
        {
            driveGain += (targetDrive - driveGain) * driveSmoothing;
            const auto driven = driveGain > 1.0001f;

            for (int channel = 0; channel < numChannels; ++channel)
            {
                auto x = block.channels[channel][i];

                // The drive saturates what goes in, its quiet parts keeping their level.
                if (driven)
                    x = dsp::softClip (x * driveGain) / driveGain;

                if (comb)
                {
                    x = combs[(size_t) channel].process (x);
                }
                else
                {
                    for (int s = 0; s < sections.count; ++s)
                        x = stages[(size_t) s][(size_t) channel].process (x, sections.coefficients[(size_t) s]);
                }

                block.channels[channel][i] = x;
            }
        }
    }

    // How long a comb rings once its input stops: at the lower of where its cutoff is and where it is gliding to, the longer.
    tailSeconds.store (comb ? (float) dsp::getRingSeconds (dsp::makeComb (getType (values) == Type::combMinus,
                                                                          std::min (toHertz (octaves), (double) values[cutoff]), sampleRate,
                                                                          values[resonance]),
                                                           sampleRate)
                            : 0.0f,
                       std::memory_order_relaxed);
}
} // namespace phazite::devices
