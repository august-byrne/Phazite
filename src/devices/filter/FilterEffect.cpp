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
    constexpr std::string_view types[] = { "Low Pass", "High Pass", "Band Pass", "Notch",        "Comb +",
                                           "Comb -",   "Vowel",     "Phaser",    "Sample & Hold" };
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

    static_assert (std::size (types) == (size_t) FilterEffect::Type::sampleAndHold + 1);

    // In a mixer slot: the sweep.
    constexpr std::string_view keyControls[] = { "cutoff", "resonance" };

    constexpr double glideSeconds = 0.01; // how long the cutoff takes to follow a change, about
    constexpr double driveSeconds = 0.02;

    dsp::FilterMode getMode (std::span<const float> values)
    {
        using Type = FilterEffect::Type;
        using dsp::FilterMode;

        switch ((Type) juce::jlimit (0, (int) std::size (types) - 1, (int) std::lround (values[FilterEffect::type])))
        {
            case Type::lowPass:
                return FilterMode::lowPass;
            case Type::highPass:
                return FilterMode::highPass;
            case Type::bandPass:
                return FilterMode::bandPass;
            case Type::notch:
                return FilterMode::notch;
            case Type::combPlus:
                return FilterMode::combPlus;
            case Type::combMinus:
                return FilterMode::combMinus;
            case Type::vowel:
                return FilterMode::vowel;
            case Type::phaser:
                return FilterMode::phaser;
            case Type::sampleAndHold:
                return FilterMode::sampleAndHold;
        }

        return FilterMode::lowPass;
    }

    // The filter at a cutoff: 12 or 24 dB per octave as the slope says (a comb, the vowel, the phaser and the sample
    // and hold have none), a 12 dB section's Q rising from a Butterworth's with the resonance.
    dsp::MultimodeCoefficients makeCoefficients (std::span<const float> values, double cutoffHz, double sampleRate)
    {
        const auto amount = (double) juce::jlimit (0.0f, 1.0f, values[FilterEffect::resonance]);
        return dsp::makeMultimode ({ .mode = getMode (values),
                                     .frequency = cutoffHz,
                                     .resonance = values[FilterEffect::resonance],
                                     .steep = values[FilterEffect::slope] > 0.5f,
                                     .q = 0.70710678 * std::pow (2.0, amount * 4.5) },
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

const DeviceInfo FilterEffect::info { .typeID = typeID,
                                      .name = "Filter",
                                      .kind = DeviceKind::effect,
                                      .parameters = parameters,
                                      .keyControls = keyControls };

double FilterEffect::getMagnitude (std::span<const float> values, double frequency, double sampleRate)
{
    return dsp::getMagnitude (makeCoefficients (values, values[cutoff], sampleRate), frequency, sampleRate);
}

void FilterEffect::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    glide = (float) (1.0 - std::exp (-1.0 / (glideSeconds * sampleRate)));
    driveSmoothing = (float) (1.0 - std::exp (-1.0 / (driveSeconds * sampleRate)));

    for (auto& filter : filters)
        filter.prepare (dsp::getLongestCombDelay (lowestCutoff, sampleRate));

    reset();
}

void FilterEffect::reset()
{
    for (auto& filter : filters)
        filter.reset();

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
        {
            driveGain += (targetDrive - driveGain) * driveSmoothing;
            const auto driven = driveGain > 1.0001f;

            for (int channel = 0; channel < numChannels; ++channel)
            {
                auto x = block.channels[channel][i];

                // The drive saturates what goes in, its quiet parts keeping their level.
                if (driven)
                    x = dsp::softClip (x * driveGain) / driveGain;

                block.channels[channel][i] = filters[(size_t) channel].process (x);
            }
        }
    }

    // How long a comb rings once its input stops: at the lower of where its cutoff is and where it is gliding to, the
    // longer. The other types fall silent within a few milliseconds.
    const auto lowest = makeCoefficients (values, std::min (toHertz (octaves), (double) values[cutoff]), sampleRate);
    const auto comb = lowest.mode == dsp::FilterMode::combPlus || lowest.mode == dsp::FilterMode::combMinus;
    tailSeconds.store (comb ? (float) dsp::getRingSeconds (lowest.comb, sampleRate) : 0.0f, std::memory_order_relaxed);
}
} // namespace phazite::devices
