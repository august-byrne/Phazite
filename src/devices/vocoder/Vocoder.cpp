// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Vocoder.h"
#include "devices/dsp/Math.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace phazite::devices
{
namespace
{
    constexpr std::string_view carriers[] = { "Buzz", "Noise", "Voice", "Track" };
    constexpr std::string_view bandCounts[] = { "8", "12", "16", "24", "32", "40" };
    constexpr int bandCountValues[] = { 8, 12, 16, 24, 32, 40 };

    // The IDs are saved in projects.
    constexpr ParameterInfo parameters[] = {
        { .id = "carrier", .name = "Carrier", .maximum = 3.0f, .defaultValue = 0.0f, .unit = Unit::choice, .choices = carriers },
        { .id = "pitch", .name = "Pitch", .minimum = -24.0f, .maximum = 24.0f, .defaultValue = 0.0f, .unit = Unit::semitones },
        { .id = "bands", .name = "Bands", .maximum = 5.0f, .defaultValue = 2.0f, .unit = Unit::choice, .choices = bandCounts },
        { .id = "lowFrequency",
          .name = "Low Frequency",
          .minimum = 40.0f,
          .maximum = 1000.0f,
          .defaultValue = 100.0f,
          .unit = Unit::hertz,
          .midpoint = 200.0f },
        { .id = "highFrequency",
          .name = "High Frequency",
          .minimum = 2000.0f,
          .maximum = 18000.0f,
          .defaultValue = 10000.0f,
          .unit = Unit::hertz,
          .midpoint = 6000.0f },
        { .id = "bandWidth", .name = "Band Width", .minimum = 0.25f, .maximum = 2.0f, .defaultValue = 1.0f, .unit = Unit::percent },
        { .id = "formant", .name = "Formant", .minimum = -12.0f, .maximum = 12.0f, .defaultValue = 0.0f, .unit = Unit::semitones },
        { .id = "attack",
          .name = "Attack",
          .minimum = 0.5f,
          .maximum = 50.0f,
          .defaultValue = 2.0f,
          .unit = Unit::milliseconds,
          .midpoint = 5.0f },
        { .id = "release",
          .name = "Release",
          .minimum = 5.0f,
          .maximum = 500.0f,
          .defaultValue = 20.0f,
          .unit = Unit::milliseconds,
          .midpoint = 50.0f },
        { .id = "unvoiced", .name = "Unvoiced", .defaultValue = 0.5f, .unit = Unit::percent },
        { .id = "brightness", .name = "Brightness", .minimum = -1.0f, .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::percent },
        { .id = "stereo", .name = "Stereo", .defaultValue = 0.0f, .unit = Unit::percent },
        { .id = "output", .name = "Output", .minimum = -24.0f, .maximum = 24.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
        { .id = mixParameterID, .name = "Mix", .defaultValue = 1.0f, .unit = Unit::percent },
    };

    // In a mixer slot: the size of the voice, and its consonants.
    constexpr std::string_view keyControls[] = { "formant", "unvoiced" };

    constexpr double buzzHertz = 130.8127826502993; // C3
    constexpr double sectionShare = 0.6435942529;   // each band's two sections' Q, of the band's: sqrt (sqrt (2) - 1)
    constexpr float closed = 3.16e-4f;              // -70 dB: a band of the voice quieter than this plays nothing
    constexpr float leastCarrier = 1.0e-3f;         // -60 dB: the quietest a carrier is evened out from
    constexpr double consonantHertz = 4000.0;       // above it, a voice's consonants
    constexpr double pinkOctaves = 6.907755;        // ln (1000): pink noise's power over 20 Hz to 20 kHz, in octaves of e
    constexpr double mostRaised = 0.25;             // 12 dB: the most a band is raised over its share of pink noise
    constexpr float meanToRms = 1.25f;              // a level as it is averaged (rectified) to its RMS, about
    constexpr float followedToRms = 1.05f;          // a voice's band's level as followed (at the first attack and release)
                                                    // to its RMS, about: measured with pink noise
    constexpr double fadeSeconds = 0.01;            // out and in, when the number of bands changes
    constexpr std::uint32_t noiseSeed = 1939;       // the noise, the same at every render (Dudley's vocoder, 1939)

    double getSectionQ (std::span<const float> values)
    {
        // Neighbouring bands meet at their -3 dB points at 100 % width.
        const auto count = Vocoder::getNumBands (values);
        const auto ratio = Vocoder::getBandFrequency (values, 1) / Vocoder::getBandFrequency (values, 0);
        const auto spread = std::sqrt (ratio) - 1.0 / std::sqrt (ratio);
        const auto q = count > 1 && spread > 0.0 ? 1.0 / (std::max (0.05, (double) values[Vocoder::bandWidth]) * spread) : 1.0;
        return std::clamp (q * sectionShare, 0.3, 40.0);
    }

    double getBuzzHertz (std::span<const float> values)
    {
        return buzzHertz * std::exp2 ((double) values[Vocoder::pitch] / 12.0);
    }
} // namespace

const DeviceInfo Vocoder::info { .typeID = typeID,
                                 .name = "Vocoder",
                                 .kind = DeviceKind::effect,
                                 .parameters = parameters,
                                 .keyControls = keyControls,
                                 .hasSidechain = true };

int Vocoder::getNumBands (std::span<const float> values)
{
    return bandCountValues[(size_t) std::clamp ((int) std::lround (values[bands]), 0, (int) std::size (bandCountValues) - 1)];
}

double Vocoder::getBandFrequency (std::span<const float> values, int band)
{
    const auto count = getNumBands (values);
    const auto low = (double) values[lowFrequency];
    const auto high = std::max ((double) values[highFrequency], 1.5 * low);
    return low * std::pow (high / low, count > 1 ? (double) band / (count - 1) : 0.0);
}

double Vocoder::getCarrierBandFrequency (std::span<const float> values, int band, double rate)
{
    return std::min (getBandFrequency (values, band) * std::exp2 ((double) values[formant] / 12.0), 0.45 * rate);
}

Vocoder::Carrier Vocoder::getCarrier (std::span<const float> values)
{
    return (Carrier) std::clamp ((int) std::lround (values[carrier]), 0, (int) Carrier::track);
}

void Vocoder::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    fadeStep = (float) (1.0 / (fadeSeconds * sampleRate));
    consonantCoefficients = dsp::makeFilter (dsp::FilterShape::highPass, consonantHertz, sampleRate, 0.70710678);

    for (auto* follower : { &highLevel, &wholeLevel })
    {
        follower->prepare (sampleRate);
        follower->setTimes (0.001f, 0.02f);
    }

    // How loud the carrier has been lately: at once as it rises, falling over a second, so that a pause in it doesn't lower
    // the floor its bands are evened out from. In each band, an average, as quick to rise as to fall.
    carrierLevel.prepare (sampleRate);
    carrierLevel.setTimes (0.01f, 1.0f);

    for (auto& band : bandsInUse)
    {
        band.follower.prepare (sampleRate);
        band.carrierFollower.prepare (sampleRate);
        band.carrierFollower.setTimes (0.02f, 0.02f);
    }

    made = false; // the bands are made at the first block
    numBands = 0;
    reset();
}

void Vocoder::reset()
{
    for (auto& band : bandsInUse)
    {
        for (auto& filter : band.voiceFilters)
            filter.reset();

        for (auto& filter : band.carrierFilters)
            filter.reset();

        band.follower.reset();
        band.carrierFollower.reset();
    }

    consonantHighPass.reset();
    highLevel.reset();
    wholeLevel.reset();
    carrierLevel.reset();
    buzz.setPhase (0.0f);
    noise.setSeed (noiseSeed);
    fade = 1.0f;
    pendingBands = 0;
}

void Vocoder::makeBands (std::span<const float> values, int count)
{
    const auto fresh = count != numBands;
    numBands = count;
    const auto sectionQ = getSectionQ (values);

    for (int k = 0; k < numBands; ++k)
    {
        auto& band = bandsInUse[(size_t) k];
        const auto hertz = getBandFrequency (values, k);
        const auto carrierHertz = getCarrierBandFrequency (values, k, sampleRate);
        band.analysis = dsp::makeFilter (dsp::FilterShape::bandPass, hertz, sampleRate, sectionQ);
        band.synthesis = dsp::makeFilter (dsp::FilterShape::bandPass, carrierHertz, sampleRate, sectionQ);

        // Each band's level followed no faster than two cycles of its centre: its level, not its wave.
        const auto shortest = (float) (2.0 / hertz);
        band.follower.setTimes (std::max (values[attack] * 0.001f, shortest), std::max (values[release] * 0.001f, shortest));

        // Brightness tilts the bands by up to 3 dB an octave around 1 kHz.
        band.gain = (float) std::pow (carrierHertz / 1000.0, 0.5 * (double) values[brightness]);
        band.pan = k % 2 == 0 ? -1.0f : 1.0f;

        // Every other band upside down: neighbouring bands are half a cycle apart where they overlap, and would cancel.
        band.polarity = k % 2 == 0 ? 1.0f : -1.0f;

        if (fresh)
        {
            for (auto& filter : band.voiceFilters)
                filter.reset();

            for (auto& filter : band.carrierFilters)
                filter.reset();

            band.follower.reset();
            band.carrierFollower.reset();
        }
    }

    // A band's share of pink noise (its RMS, of the whole's): its noise-equivalent width, pi f / (4 Q) for its two sections,
    // of the octaves of e that pink noise spreads its power over.
    bandFloor = (float) (mostRaised * std::sqrt (juce::MathConstants<double>::pi / (4.0 * sectionQ) / pinkOctaves));

    // How long it sounds on: its slowest band's release, falling by 60 dB.
    const auto slowest = std::max ((double) values[release] * 0.001, 2.0 / getBandFrequency (values, 0));
    tailSeconds.store (6.9 * slowest + 0.02, std::memory_order_relaxed);
}

float Vocoder::getCarrierSample (Carrier kind, float voice, const ProcessContext& block, int sample)
{
    switch (kind)
    {
        case Carrier::buzz:
            return buzz.next (dsp::Waveform::saw);
        case Carrier::noise:
            return noise.nextBipolar();
        case Carrier::voice:
            return voice;
        case Carrier::track:
            if (block.sidechain == nullptr || block.numSidechainChannels <= 0)
                return 0.0f;

            return block.numSidechainChannels > 1 ? 0.5f * (block.sidechain[0][sample] + block.sidechain[1][sample])
                                                  : block.sidechain[0][sample];
    }

    return 0.0f;
}

void Vocoder::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);

    if (numChannels <= 0)
        return;

    // The bands, made again when what they depend on changes; when their number changes, the sound fades out first.
    decltype (madeFrom) wanted;

    for (size_t i = 0; i < wanted.size(); ++i)
        wanted[i] = values[(size_t) shapingParameters[i]];

    if (! made || std::memcmp (wanted.data(), madeFrom.data(), sizeof (wanted)) != 0)
    {
        const auto count = getNumBands (values);
        const auto countChanges = numBands > 0 && count != numBands;
        madeFrom = wanted;
        made = true;

        // A change back before the fade is over cancels the one waiting.
        pendingBands = countChanges ? count : 0;

        if (! countChanges)
            makeBands (values, count);
    }

    const auto kind = getCarrier (values);
    const auto unvoicedAmount = values[unvoiced];
    const auto widthAmount = values[stereo];
    const auto outputGain = dsp::decibelsToGain (values[output]);
    buzz.setFrequency (getBuzzHertz (values), sampleRate);

    auto* left = block.channels[0];
    auto* right = numChannels > 1 ? block.channels[1] : nullptr;
    auto loudestConsonant = 0.0f;

    for (int i = 0; i < block.numSamples; ++i)
    {
        // A change in the number of bands: out, the new bands, and in again.
        if (pendingBands > 0)
        {
            fade -= fadeStep;

            if (fade <= 0.0f)
            {
                fade = 0.0f;
                makeBands (values, pendingBands);
                pendingBands = 0;
            }
        }
        else if (fade < 1.0f)
        {
            fade = std::min (1.0f, fade + fadeStep);
        }

        const auto voice = right != nullptr ? 0.5f * (left[i] + right[i]) : left[i];

        // Consonants: how much of the voice is above 4 kHz, from none at 30 % to all at 60 %.
        const auto high = highLevel.process (std::abs (consonantHighPass.process (voice, consonantCoefficients)));
        const auto whole = wholeLevel.process (std::abs (voice));
        const auto consonant = whole > closed ? std::clamp ((high / whole - 0.3f) / 0.3f, 0.0f, 1.0f) : 0.0f;
        loudestConsonant = std::max (loudestConsonant, consonant);

        auto carrierSample = getCarrierSample (kind, voice, block, i);

        // The consonants' noise, as loud as the carrier has been lately, so that a silent carrier stays silent.
        const auto lately = carrierLevel.process (std::abs (carrierSample)) * meanToRms;
        carrierSample += unvoicedAmount * consonant * lately * noise.nextBipolar();
        const auto floor = std::max (lately * bandFloor, leastCarrier);

        for (int k = 0; k < numBands; ++k)
        {
            auto& band = bandsInUse[(size_t) k];
            band.played = band.carrierFilters[1].process (band.carrierFilters[0].process (carrierSample, band.synthesis), band.synthesis);
            band.carried = band.carrierFollower.process (std::abs (band.played)) * meanToRms;
        }

        auto sumLeft = 0.0f, sumRight = 0.0f;

        for (int k = 0; k < numBands; ++k)
        {
            auto& band = bandsInUse[(size_t) k];
            const auto voiced = band.voiceFilters[1].process (band.voiceFilters[0].process (voice, band.analysis), band.analysis);
            const auto level = band.follower.process (std::abs (voiced)) * followedToRms;

            if (level < closed)
                continue;

            // The carrier evened out by the louder of the band and its neighbours, above the floor.
            auto carried = std::max (band.carried, floor);

            if (k > 0)
                carried = std::max (carried, bandsInUse[(size_t) k - 1].carried);

            if (k + 1 < numBands)
                carried = std::max (carried, bandsInUse[(size_t) k + 1].carried);

            const auto y = band.polarity * band.played * level * band.gain / carried;
            const auto pan = band.pan * widthAmount;
            sumLeft += (1.0f - pan) * y;
            sumRight += (1.0f + pan) * y;
        }

        const auto gain = outputGain * fade;

        if (right != nullptr)
        {
            left[i] = sumLeft * gain;
            right[i] = sumRight * gain;
        }
        else
        {
            left[i] = 0.5f * (sumLeft + sumRight) * gain;
        }
    }

    for (int k = 0; k < maxBands; ++k)
        bandLevels[(size_t) k].set (k < numBands ? bandsInUse[(size_t) k].follower.getValue() * followedToRms : 0.0f);

    shownCarrierLevel.set (carrierLevel.getValue() * meanToRms);
    shownConsonant.set (loudestConsonant);
}
} // namespace phazite::devices
