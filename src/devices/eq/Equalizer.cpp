// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Equalizer.h"
#include "devices/dsp/Math.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
namespace
{
    using BandType = Equalizer::BandType;

    constexpr std::string_view bandTypes[] = { "Low Cut", "Low Shelf", "Bell", "Notch", "High Shelf", "High Cut" };
    constexpr std::string_view slopes[] = { "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct" };

    // The bands' parameter IDs, saved in projects (never change them), and names.
    constexpr std::string_view onIDs[] = { "band1On", "band2On", "band3On", "band4On", "band5On", "band6On", "band7On", "band8On" };
    constexpr std::string_view typeIDs[] = { "band1Type", "band2Type", "band3Type", "band4Type",
                                             "band5Type", "band6Type", "band7Type", "band8Type" };
    constexpr std::string_view frequencyIDs[] = { "band1Frequency", "band2Frequency", "band3Frequency", "band4Frequency",
                                                  "band5Frequency", "band6Frequency", "band7Frequency", "band8Frequency" };
    constexpr std::string_view gainIDs[] = { "band1Gain", "band2Gain", "band3Gain", "band4Gain",
                                             "band5Gain", "band6Gain", "band7Gain", "band8Gain" };
    constexpr std::string_view qIDs[] = { "band1Q", "band2Q", "band3Q", "band4Q", "band5Q", "band6Q", "band7Q", "band8Q" };
    constexpr std::string_view slopeIDs[] = { "band1Slope", "band2Slope", "band3Slope", "band4Slope",
                                              "band5Slope", "band6Slope", "band7Slope", "band8Slope" };

    constexpr std::string_view onNames[] = { "Band 1 On", "Band 2 On", "Band 3 On", "Band 4 On",
                                             "Band 5 On", "Band 6 On", "Band 7 On", "Band 8 On" };
    constexpr std::string_view typeNames[] = { "Band 1 Type", "Band 2 Type", "Band 3 Type", "Band 4 Type",
                                               "Band 5 Type", "Band 6 Type", "Band 7 Type", "Band 8 Type" };
    constexpr std::string_view frequencyNames[] = { "Band 1 Frequency", "Band 2 Frequency", "Band 3 Frequency", "Band 4 Frequency",
                                                    "Band 5 Frequency", "Band 6 Frequency", "Band 7 Frequency", "Band 8 Frequency" };
    constexpr std::string_view gainNames[] = { "Band 1 Gain", "Band 2 Gain", "Band 3 Gain", "Band 4 Gain",
                                               "Band 5 Gain", "Band 6 Gain", "Band 7 Gain", "Band 8 Gain" };
    constexpr std::string_view qNames[] = {
        "Band 1 Q", "Band 2 Q", "Band 3 Q", "Band 4 Q", "Band 5 Q", "Band 6 Q", "Band 7 Q", "Band 8 Q"
    };
    constexpr std::string_view slopeNames[] = { "Band 1 Slope", "Band 2 Slope", "Band 3 Slope", "Band 4 Slope",
                                                "Band 5 Slope", "Band 6 Slope", "Band 7 Slope", "Band 8 Slope" };

    // A new EQ: cuts off at the ends of the range, and the other bands flat across it.
    struct BandDefaults
    {
        BandType type;
        float frequency, q;
        bool on;
    };

    constexpr BandDefaults defaults[] = { { BandType::lowCut, 30.0f, 0.71f, false },      { BandType::lowShelf, 100.0f, 0.71f, true },
                                          { BandType::bell, 250.0f, 1.0f, true },         { BandType::bell, 800.0f, 1.0f, true },
                                          { BandType::bell, 2500.0f, 1.0f, true },        { BandType::bell, 6000.0f, 1.0f, true },
                                          { BandType::highShelf, 10000.0f, 0.71f, true }, { BandType::highCut, 18000.0f, 0.71f, false } };

    constexpr auto makeParameters()
    {
        std::array<ParameterInfo, Equalizer::outputParameter + 1> made {};

        for (int band = 0; band < Equalizer::numBands; ++band)
        {
            const auto& d = defaults[band];
            const auto at = [band] (Equalizer::Field field) { return (size_t) Equalizer::getParameterIndex (band, field); };

            made[at (Equalizer::on)] = { .id = onIDs[band],
                                         .name = onNames[band],
                                         .defaultValue = d.on ? 1.0f : 0.0f,
                                         .unit = Unit::toggle };
            made[at (Equalizer::type)] = { .id = typeIDs[band],
                                           .name = typeNames[band],
                                           .minimum = 0.0f,
                                           .maximum = 5.0f,
                                           .defaultValue = (float) d.type,
                                           .unit = Unit::choice,
                                           .choices = bandTypes };
            made[at (Equalizer::frequency)] = { .id = frequencyIDs[band],
                                                .name = frequencyNames[band],
                                                .minimum = 20.0f,
                                                .maximum = 20000.0f,
                                                .defaultValue = d.frequency,
                                                .unit = Unit::hertz,
                                                .midpoint = 632.456f }; // the middle of the range, on a log scale
            made[at (Equalizer::gain)] = { .id = gainIDs[band],
                                           .name = gainNames[band],
                                           .minimum = -24.0f,
                                           .maximum = 24.0f,
                                           .defaultValue = 0.0f,
                                           .unit = Unit::decibels };
            made[at (Equalizer::q)] = { .id = qIDs[band],
                                        .name = qNames[band],
                                        .minimum = 0.1f,
                                        .maximum = 18.0f,
                                        .defaultValue = d.q,
                                        .midpoint = 1.0f };
            made[at (Equalizer::slope)] = { .id = slopeIDs[band],
                                            .name = slopeNames[band],
                                            .minimum = 0.0f,
                                            .maximum = 3.0f,
                                            .defaultValue = 1.0f,
                                            .unit = Unit::choice,
                                            .choices = slopes };
        }

        made[Equalizer::outputParameter] = { .id = "output",
                                             .name = "Output",
                                             .minimum = -24.0f,
                                             .maximum = 24.0f,
                                             .defaultValue = 0.0f,
                                             .unit = Unit::decibels };
        return made;
    }

    constexpr auto parameters = makeParameters();

    constexpr int stretch = 32;               // samples between updates of moving coefficients
    constexpr double smoothingSeconds = 0.02; // how long a parameter's change takes to settle, about

    bool isShelfOrBell (int type)
    {
        return type == (int) BandType::lowShelf || type == (int) BandType::bell || type == (int) BandType::highShelf;
    }

    // A band's filter sections. A cut is a Butterworth slope whose steepest section takes the band's Q relative to the
    // flat 0.71, so that its default is flat and more Q gives it a resonant bump at its frequency.
    int makeSections (int type, int slope, double frequency, double gainDecibels, double q, double sampleRate,
                      std::array<dsp::FilterCoefficients, 4>& sections)
    {
        using dsp::FilterShape;

        switch ((BandType) type)
        {
            case BandType::lowCut:
            case BandType::highCut:
            {
                const auto order = 2 * (std::clamp (slope, 0, 3) + 1);
                const auto numSections = order / 2;

                for (int section = 0; section < numSections; ++section)
                {
                    auto sectionQ = dsp::getButterworthQ (order, section);

                    if (section == numSections - 1)
                        sectionQ *= q / 0.70710678;

                    sections[(size_t) section] = dsp::makeFilter (
                        type == (int) BandType::lowCut ? FilterShape::highPass : FilterShape::lowPass, frequency, sampleRate, sectionQ);
                }

                return numSections;
            }

            case BandType::lowShelf:
                sections[0] = dsp::makeFilter (FilterShape::lowShelf, frequency, sampleRate, q, gainDecibels);
                return 1;

            case BandType::bell:
                sections[0] = dsp::makeFilter (FilterShape::bell, frequency, sampleRate, q, gainDecibels);
                return 1;

            case BandType::notch:
                sections[0] = dsp::makeFilter (FilterShape::notch, frequency, sampleRate, q);
                return 1;

            case BandType::highShelf:
                sections[0] = dsp::makeFilter (FilterShape::highShelf, frequency, sampleRate, q, gainDecibels);
                return 1;
        }

        return 0;
    }

    bool isNear (float a, float b, float tolerance)
    {
        return std::abs (a - b) <= tolerance;
    }
} // namespace

const DeviceInfo Equalizer::info { .typeID = typeID, .name = "EQ", .kind = DeviceKind::effect, .parameters = parameters };

double Equalizer::getBandMagnitude (std::span<const float> values, int band, double frequency, double sampleRate)
{
    const auto at = [&values, band] (Field field) { return values[(size_t) getParameterIndex (band, field)]; };

    if (at (on) < 0.5f)
        return 1.0;

    std::array<dsp::FilterCoefficients, maxSections> sections {};
    const auto numSections = makeSections ((int) std::lround (at (type)), (int) std::lround (at (slope)), at (Field::frequency), at (gain),
                                           at (q), sampleRate, sections);
    auto magnitude = 1.0;

    for (int section = 0; section < numSections; ++section)
        magnitude *= dsp::getMagnitude (sections[(size_t) section], frequency, sampleRate);

    return magnitude;
}

double Equalizer::getMagnitude (std::span<const float> values, double frequency, double sampleRate)
{
    auto magnitude = (double) dsp::decibelsToGain (values[outputParameter]);

    for (int band = 0; band < numBands; ++band)
        magnitude *= getBandMagnitude (values, band, frequency, sampleRate);

    return magnitude;
}

void Equalizer::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    smoothing = (float) (1.0 - std::exp (-stretch / (smoothingSeconds * sampleRate)));
    reset();
}

void Equalizer::reset()
{
    for (auto& band : bands)
    {
        band = Band {};
        band.type = -1; // so the first block makes every band from its parameters
    }

    outputGain = -1.0f;
}

void Equalizer::update (Band& band, int index, std::span<const float> values, bool jump)
{
    const auto at = [&values, index] (Field field) { return values[(size_t) getParameterIndex (index, field)]; };
    const auto newOn = at (on) > 0.5f;
    const auto newType = (int) std::lround (at (type)), newSlope = (int) std::lround (at (slope));
    const auto targetFrequency = at (Field::frequency), targetGain = at (gain), targetQ = at (q);

    // Another filter: it starts from silence, at its settings.
    if (newOn != band.on || newType != band.type || newSlope != band.slope)
    {
        band.on = newOn;
        band.type = newType;
        band.slope = newSlope;
        jump = true;

        for (auto& channel : band.filters)
            for (auto& filter : channel)
                filter.reset();
    }

    if (! band.on)
    {
        band.numSections = 0;
        return;
    }

    const auto settled = isNear (band.frequency, targetFrequency, targetFrequency * 1.0e-4f) && isNear (band.gain, targetGain, 1.0e-3f)
                         && isNear (band.q, targetQ, targetQ * 1.0e-4f);

    if (settled && ! jump)
        return;

    if (jump || settled)
    {
        band.frequency = targetFrequency;
        band.gain = targetGain;
        band.q = targetQ;
    }
    else
    {
        // Frequency and Q move on a log scale, as the ear hears them.
        band.frequency *= std::pow (targetFrequency / band.frequency, smoothing);
        band.gain += (targetGain - band.gain) * smoothing;
        band.q *= std::pow (targetQ / band.q, smoothing);
    }

    // A flat shelf or bell changes nothing, so it isn't played; it starts from silence when it bends.
    const auto wasPlayed = band.numSections > 0;
    band.numSections = isShelfOrBell (band.type) && std::abs (band.gain) < 0.001f
                           ? 0
                           : makeSections (band.type, band.slope, band.frequency, band.gain, band.q, sampleRate, band.sections);

    if (! wasPlayed && band.numSections > 0)
        for (auto& channel : band.filters)
            for (auto& filter : channel)
                filter.reset();
}

void Equalizer::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto numChannels = std::min (block.numChannels, 2);
    const auto targetOutput = dsp::decibelsToGain (block.parameters[(size_t) outputParameter]);

    for (int start = 0; start < block.numSamples; start += stretch)
    {
        const auto length = std::min (stretch, block.numSamples - start);

        for (int index = 0; index < numBands; ++index)
        {
            auto& band = bands[(size_t) index];
            update (band, index, block.parameters, false);

            for (int channel = 0; channel < numChannels; ++channel)
            {
                auto* data = block.channels[channel] + start;

                for (int section = 0; section < band.numSections; ++section)
                {
                    auto& filter = band.filters[(size_t) channel][(size_t) section];
                    const auto& coefficients = band.sections[(size_t) section];

                    for (int i = 0; i < length; ++i)
                        data[i] = filter.process (data[i], coefficients);
                }
            }
        }

        // The output gain, ramped across the stretch.
        const auto from = outputGain < 0.0f ? targetOutput : outputGain;
        outputGain = isNear (from, targetOutput, 1.0e-5f) ? targetOutput : from + (targetOutput - from) * smoothing;

        for (int channel = 0; channel < numChannels; ++channel)
        {
            auto* data = block.channels[channel] + start;

            for (int i = 0; i < length; ++i)
                data[i] *= from + (outputGain - from) * (float) (i + 1) / (float) length;
        }
    }

    if (numChannels > 0)
        outputTap.push (block.channels[0], block.channels[numChannels - 1], block.numSamples);
}
} // namespace phazite::devices
