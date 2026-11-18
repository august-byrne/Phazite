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
    constexpr std::string_view placements[] = { "Stereo", "Mid", "Side", "Left", "Right" };

    // The bands' parameter IDs, saved in projects, and names.
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
    constexpr std::string_view placementIDs[] = { "band1Placement", "band2Placement", "band3Placement", "band4Placement",
                                                  "band5Placement", "band6Placement", "band7Placement", "band8Placement" };
    constexpr std::string_view dynamicIDs[] = { "band1Dynamic", "band2Dynamic", "band3Dynamic", "band4Dynamic",
                                                "band5Dynamic", "band6Dynamic", "band7Dynamic", "band8Dynamic" };
    constexpr std::string_view thresholdIDs[] = { "band1Threshold", "band2Threshold", "band3Threshold", "band4Threshold",
                                                  "band5Threshold", "band6Threshold", "band7Threshold", "band8Threshold" };
    constexpr std::string_view placementNames[] = { "Band 1 Placement", "Band 2 Placement", "Band 3 Placement", "Band 4 Placement",
                                                    "Band 5 Placement", "Band 6 Placement", "Band 7 Placement", "Band 8 Placement" };
    constexpr std::string_view dynamicNames[] = { "Band 1 Dynamic", "Band 2 Dynamic", "Band 3 Dynamic", "Band 4 Dynamic",
                                                  "Band 5 Dynamic", "Band 6 Dynamic", "Band 7 Dynamic", "Band 8 Dynamic" };
    constexpr std::string_view thresholdNames[] = { "Band 1 Threshold", "Band 2 Threshold", "Band 3 Threshold", "Band 4 Threshold",
                                                    "Band 5 Threshold", "Band 6 Threshold", "Band 7 Threshold", "Band 8 Threshold" };

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
        std::array<ParameterInfo, Equalizer::mixParameter + 1> made {};

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
                                                .scale = Scale::logarithmic };
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
            made[at (Equalizer::placement)] = { .id = placementIDs[band],
                                                .name = placementNames[band],
                                                .minimum = 0.0f,
                                                .maximum = 4.0f,
                                                .defaultValue = (float) Equalizer::Placement::stereo,
                                                .unit = Unit::choice,
                                                .choices = placements };
            made[at (Equalizer::dynamic)] = { .id = dynamicIDs[band],
                                              .name = dynamicNames[band],
                                              .minimum = -24.0f,
                                              .maximum = 24.0f,
                                              .defaultValue = 0.0f,
                                              .unit = Unit::decibels };
            made[at (Equalizer::threshold)] = { .id = thresholdIDs[band],
                                                .name = thresholdNames[band],
                                                .minimum = -60.0f,
                                                .maximum = 0.0f,
                                                .defaultValue = -24.0f,
                                                .unit = Unit::decibels };
        }

        made[Equalizer::outputParameter] = { .id = "output",
                                             .name = "Output",
                                             .minimum = -24.0f,
                                             .maximum = 24.0f,
                                             .defaultValue = 0.0f,
                                             .unit = Unit::decibels };
        made[Equalizer::mixParameter] = { .id = mixParameterID, .name = "Mix", .defaultValue = 1.0f, .unit = Unit::percent };
        return made;
    }

    constexpr auto parameters = makeParameters();

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

                    sections[(size_t) section] = dsp::makeMatchedFilter (
                        type == (int) BandType::lowCut ? FilterShape::highPass : FilterShape::lowPass, frequency, sampleRate, sectionQ);
                }

                return numSections;
            }

            case BandType::lowShelf:
                sections[0] = dsp::makeMatchedFilter (FilterShape::lowShelf, frequency, sampleRate, q, gainDecibels);
                return 1;

            case BandType::bell:
                sections[0] = dsp::makeMatchedFilter (FilterShape::bell, frequency, sampleRate, q, gainDecibels);
                return 1;

            case BandType::notch:
                sections[0] = dsp::makeMatchedFilter (FilterShape::notch, frequency, sampleRate, q);
                return 1;

            case BandType::highShelf:
                sections[0] = dsp::makeMatchedFilter (FilterShape::highShelf, frequency, sampleRate, q, gainDecibels);
                return 1;
        }

        return 0;
    }

    bool isNear (float a, float b, float tolerance)
    {
        return std::abs (a - b) <= tolerance;
    }

    int toPlacement (float value)
    {
        return std::clamp ((int) std::lround (value), (int) Equalizer::Placement::stereo, (int) Equalizer::Placement::right);
    }

    // What a dynamic band hears of the sound: a bell's band, at least an octave or so wide (a Q of 1 or more), or what a
    // shelf raises or lowers, 12 dB an octave beyond its frequency.
    dsp::FilterCoefficients makeEar (int type, double frequency, double q, double sampleRate)
    {
        using dsp::FilterShape;

        if (type == (int) BandType::bell)
            return dsp::makeFilter (FilterShape::bandPass, frequency, sampleRate, std::max (q, 1.0));

        return dsp::makeFilter (type == (int) BandType::lowShelf ? FilterShape::lowPass : FilterShape::highPass, frequency, sampleRate,
                                0.70710678);
    }

    constexpr double fadeSeconds = 0.01; // a change of placement's crossfade

    static_assert (std::atomic<double>::is_always_lock_free);
} // namespace

const DeviceInfo Equalizer::info { .typeID = typeID, .name = "EQ", .kind = DeviceKind::effect, .parameters = parameters };

double Equalizer::BandShape::getMagnitude (double frequency, double sampleRate) const
{
    auto magnitude = 1.0;

    for (int section = 0; section < numSections; ++section)
        magnitude *= dsp::getMagnitude (sections[(size_t) section], frequency, sampleRate);

    return magnitude;
}

Equalizer::BandShape Equalizer::getBandShape (std::span<const float> values, int band, double sampleRate)
{
    const auto at = [&values, band] (Field field) { return values[(size_t) getParameterIndex (band, field)]; };
    BandShape shape;

    if (at (on) >= 0.5f)
        shape.numSections = makeSections ((int) std::lround (at (type)), (int) std::lround (at (slope)), at (Field::frequency), at (gain),
                                          at (q), sampleRate, shape.sections);

    return shape;
}

double Equalizer::getBandMagnitude (std::span<const float> values, int band, double frequency, double sampleRate, Placement way)
{
    if (! actsOn ((Placement) toPlacement (values[(size_t) getParameterIndex (band, placement)]), way))
        return 1.0;

    return getBandShape (values, band, sampleRate).getMagnitude (frequency, sampleRate);
}

double Equalizer::getMagnitude (std::span<const float> values, double frequency, double sampleRate, Placement way)
{
    auto magnitude = (double) dsp::decibelsToGain (values[outputParameter]);

    for (int band = 0; band < numBands; ++band)
        magnitude *= getBandMagnitude (values, band, frequency, sampleRate, way);

    return magnitude;
}

void Equalizer::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    smoothing = (float) (1.0 - std::exp (-stretch / (smoothingSeconds * sampleRate)));
    fadeSamples = std::max (1, (int) std::lround (fadeSeconds * sampleRate));
    earRise = (float) std::exp (-1.0 / (earRiseSeconds * sampleRate));
    earFall = (float) std::exp (-1.0 / (earFallSeconds * sampleRate));
    samplesPerPeak = std::max (1, (int) std::lround (sampleRate * 0.001));
    reset();
}

void Equalizer::reset()
{
    for (auto& band : bands)
    {
        band = Band {};
        band.type = -1; // so the first block makes every band from its parameters
    }

    for (auto& moved : gainsMoved)
        moved.set (0.0f);

    outputGain = -1.0f;
    anyHearing = false;
    tailSeconds.store (0.0, std::memory_order_relaxed);
    grid.reset();
}

void Equalizer::update (Band& band, int index, std::span<const float> values, bool jump)
{
    const auto at = [&values, index] (Field field) { return values[(size_t) getParameterIndex (index, field)]; };
    const auto newOn = at (on) > 0.5f;
    const auto newType = (int) std::lround (at (type)), newSlope = (int) std::lround (at (slope));
    const auto newPlacement = toPlacement (at (placement));
    const auto targetFrequency = at (Field::frequency), targetGain = at (gain), targetQ = at (q);
    const auto targetDynamic = isShelfOrBell (newType) ? at (dynamic) : 0.0f, targetThreshold = at (threshold);

    // Another filter: it starts from silence, at its settings, and so does its ear.
    if (newOn != band.on || newType != band.type || newSlope != band.slope)
    {
        band.on = newOn;
        band.type = newType;
        band.slope = newSlope;
        band.placement = newPlacement;
        band.fadeLeft = 0;
        band.hearing = false;
        jump = true;

        for (auto& channel : band.filters)
            for (auto& filter : channel)
                filter.reset();
    }
    else if (newPlacement != band.placement && band.fadeLeft <= 0)
    {
        // The band on its new placement starts from rest, so while it plays the band as it played fades out over 10 ms;
        // its ear starts from rest too, while the level it heard falls as it does. A change while a fade is under way waits for
        // its end (the placement is read again at each step), so that no fade is cut short.
        if (band.numSections > 0)
        {
            band.fadingFilters = band.filters;
            band.fadingPlacement = band.placement;
            band.fadeLeft = fadeSamples;
        }

        band.placement = newPlacement;

        for (auto& channel : band.filters)
            for (auto& filter : channel)
                filter.reset();

        for (auto& filter : band.ear)
            filter.reset();
    }

    if (! band.on)
    {
        band.numSections = 0;
        band.hearing = false;
        return;
    }

    // Frequency and Q move on a log scale, as the ear hears them.
    const auto settled = isNear (band.frequency, targetFrequency, targetFrequency * 1.0e-4f) && isNear (band.gain, targetGain, 1.0e-3f)
                         && isNear (band.q, targetQ, targetQ * 1.0e-4f);
    const auto moved = jump || ! settled;

    if (jump)
    {
        band.frequency = targetFrequency;
        band.gain = targetGain;
        band.q = targetQ;
    }
    else if (! settled)
    {
        band.frequency *= std::pow (targetFrequency / band.frequency, smoothing);
        band.gain += (targetGain - band.gain) * smoothing;
        band.q *= std::pow (targetQ / band.q, smoothing);
    }

    // A dynamic band's Dynamic and Threshold glide as its Gain does.
    band.dynamic =
        jump || isNear (band.dynamic, targetDynamic, 1.0e-3f) ? targetDynamic : band.dynamic + (targetDynamic - band.dynamic) * smoothing;
    band.threshold = jump || isNear (band.threshold, targetThreshold, 1.0e-3f)
                         ? targetThreshold
                         : band.threshold + (targetThreshold - band.threshold) * smoothing;

    // Its gain, moved by as much of its Dynamic as the level it hears is over its Threshold, up to all of it dynamicRange
    // above: from what it heard up to this step, and on to the next.
    auto played = band.gain;
    const auto wasHearing = band.hearing; // a band that starts or stops hearing is made again, played at 0 dB or not

    if (std::abs (band.dynamic) > 0.0f)
    {
        if (! wasHearing)
        {
            for (auto& filter : band.ear)
                filter.reset();

            band.peaks = {};
            band.peakIndex = band.peakSamples = 0;
            band.peakNow = band.peakBefore = band.level = 0.0f;
        }

        if (moved || ! wasHearing)
            band.earShape = makeEar (band.type, band.frequency, band.q, sampleRate);

        band.hearing = true;
        band.floorLevel = dsp::decibelsToGain (band.threshold - earFloorUnderThreshold);
        const auto heardDecibels = dsp::gainToDecibels (std::max (band.level, 1.0e-6f));
        played += band.dynamic * std::clamp ((heardDecibels - band.threshold) / dynamicRange, 0.0f, 1.0f);

        if (! moved && wasHearing && isNear (played, band.played, 1.0e-4f))
            return;
    }
    else
    {
        band.hearing = false;

        if (! moved && ! wasHearing && juce::exactlyEqual (played, band.played))
            return;
    }

    // A flat shelf or bell changes nothing, so it isn't played; it starts from silence when it bends. But a dynamic band is
    // played at 0 dB too, so that its filter is under way when its gain leaves 0: on a burst its gain leaves 0 by a decibel or
    // more within a step, and a filter starting from rest there would click.
    band.played = played;
    const auto wasPlayed = band.numSections > 0;
    band.numSections = isShelfOrBell (band.type) && std::abs (played) < 0.001f && ! band.hearing
                           ? 0
                           : makeSections (band.type, band.slope, band.frequency, played, band.q, sampleRate, band.sections);

    if (band.numSections == 0)
        band.fadeLeft = 0;

    if (! wasPlayed && band.numSections > 0)
        for (auto& channel : band.filters)
            for (auto& filter : channel)
                filter.reset();
}

void Equalizer::hear (Band& band, int numChannels, int length)
{
    const auto* left = input[0].data();
    const auto* right = input[numChannels > 1 ? 1 : 0].data();
    const auto stereo = numChannels > 1;

    // On Stereo, the louder of its two sides.
    if (band.placement == (int) Placement::stereo && stereo)
    {
        for (int i = 0; i < length; ++i)
        {
            const auto l = band.ear[0].process (left[i], band.earShape), r = band.ear[1].process (right[i], band.earShape);
            follow (band, std::max (std::abs (l), std::abs (r)));
        }

        return;
    }

    // Otherwise what it plays on (on a mono channel, the channel for Stereo, Mid and Left; nothing for Side and Right).
    switch ((Placement) band.placement)
    {
        case Placement::stereo:
        case Placement::left:
            std::copy (left, left + length, earInput.begin());
            break;

        case Placement::right:
            if (stereo)
                std::copy (right, right + length, earInput.begin());
            else
                std::fill (earInput.begin(), earInput.begin() + length, 0.0f);
            break;

        case Placement::mid:
            for (int i = 0; i < length; ++i)
                earInput[(size_t) i] = stereo ? 0.5f * (left[i] + right[i]) : left[i];
            break;

        case Placement::side:
            for (int i = 0; i < length; ++i)
                earInput[(size_t) i] = stereo ? 0.5f * (left[i] - right[i]) : 0.0f;
            break;
    }

    for (int i = 0; i < length; ++i)
        follow (band, std::abs (band.ear[0].process (earInput[(size_t) i], band.earShape)));
}

void Equalizer::follow (Band& band, float heard) const noexcept
{
    // The loudest of the millisecond going on, and of the whole ones before it (as the compressor's Duck holds its key).
    band.peakNow = std::max (band.peakNow, heard);

    if (++band.peakSamples >= samplesPerPeak)
    {
        band.peaks[(size_t) band.peakIndex] = band.peakNow;
        band.peakIndex = (band.peakIndex + 1) % earHoldMilliseconds;
        band.peakSamples = 0;
        band.peakNow = 0.0f;
        band.peakBefore = *std::max_element (band.peaks.begin(), band.peaks.end());
    }

    const auto held = std::max (band.peakBefore, band.peakNow);
    band.level = held + (band.level - held) * (held > band.level ? earRise : earFall);

    if (band.level < band.floorLevel && held < band.floorLevel)
        band.level = 0.0f;
}

void Equalizer::playOn (int where, Filters& filters, const Band& band, int numChannels, float* const* channels, int length)
{
    const auto run = [&band, length] (float* data, std::array<dsp::Filter, maxSections>& channelFilters)
    {
        for (int section = 0; section < band.numSections; ++section)
        {
            auto& filter = channelFilters[(size_t) section];
            const auto& coefficients = band.sections[(size_t) section];

            for (int i = 0; i < length; ++i)
                data[i] = filter.process (data[i], coefficients);
        }
    };

    switch ((Placement) where)
    {
        case Placement::stereo:
            for (int channel = 0; channel < numChannels; ++channel)
                run (channels[channel], filters[(size_t) channel]);
            break;

        case Placement::left:
            run (channels[0], filters[0]);
            break;

        case Placement::right:
            if (numChannels > 1)
                run (channels[1], filters[1]);
            break;

        case Placement::mid:
        case Placement::side:
        {
            // On a mono channel, the middle is the channel and there is no side.
            if (numChannels < 2)
            {
                if (where == (int) Placement::mid)
                    run (channels[0], filters[0]);

                break;
            }

            auto* left = channels[0];
            auto* right = channels[1];
            auto& middle = midSide[0];
            auto& side = midSide[1];

            for (int i = 0; i < length; ++i)
            {
                middle[(size_t) i] = 0.5f * (left[i] + right[i]);
                side[(size_t) i] = 0.5f * (left[i] - right[i]);
            }

            run (where == (int) Placement::mid ? middle.data() : side.data(), filters[0]);

            for (int i = 0; i < length; ++i)
            {
                left[i] = middle[(size_t) i] + side[(size_t) i];
                right[i] = middle[(size_t) i] - side[(size_t) i];
            }

            break;
        }
    }
}

void Equalizer::play (Band& band, int numChannels, float* const* channels, int length)
{
    if (band.numSections == 0)
        return;

    if (band.fadeLeft <= 0)
    {
        playOn (band.placement, band.filters, band, numChannels, channels, length);
        return;
    }

    // A change of placement under way: the band as it played, on a copy, fading out as the band as it plays now fades in.
    float* fadingChannels[2] = { fading[0].data(), fading[1].data() };

    for (int channel = 0; channel < numChannels; ++channel)
        std::copy (channels[channel], channels[channel] + length, fading[(size_t) channel].begin());

    playOn (band.fadingPlacement, band.fadingFilters, band, numChannels, fadingChannels, length);
    playOn (band.placement, band.filters, band, numChannels, channels, length);
    const auto faded = fadeSamples - band.fadeLeft;

    for (int channel = 0; channel < numChannels; ++channel)
    {
        auto* data = channels[channel];
        const auto* before = fading[(size_t) channel].data();

        for (int i = 0; i < length; ++i)
        {
            const auto t = std::min (1.0f, (float) (faded + i + 1) / (float) fadeSamples);
            data[i] = before[i] + (data[i] - before[i]) * t;
        }
    }

    band.fadeLeft = std::max (0, band.fadeLeft - length);
}

void Equalizer::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    silenceWhatIsNotSound (block);
    const auto numChannels = std::min (block.numChannels, 2);
    const auto targetOutput = dsp::decibelsToGain (block.parameters[(size_t) outputParameter]);

    // A stretch at a time, on the song's grid of them (dsp/ControlGrid.h): its bands move, and its output gain is worked out,
    // as a stretch starts.
    grid.startBlock (block.isPlaying, block.songSample);

    for (int start = 0; start < block.numSamples;)
    {
        if (grid.startsStep())
        {
            grid.startStep();
            anyHearing = false;
            auto lowestThreshold = 0.0f;

            for (int index = 0; index < numBands; ++index)
            {
                auto& band = bands[(size_t) index];
                update (band, index, block.parameters, false);

                if (band.hearing)
                    lowestThreshold = anyHearing ? std::min (lowestThreshold, band.threshold) : band.threshold;

                anyHearing = anyHearing || band.hearing;
            }

            // Its tail: while a band hears, its level's fall to its floor, after the loudest it heard is held.
            const auto tail = anyHearing ? (earHoldMilliseconds + 1) * 0.001
                                               + earFallSeconds * std::log (10.0) / 20.0
                                                     * (double) (earHeadroom - (lowestThreshold - earFloorUnderThreshold))
                                         : 0.0;

            if (! juce::exactlyEqual (tail, tailSeconds.load (std::memory_order_relaxed)))
                tailSeconds.store (tail, std::memory_order_relaxed);

            // From where the last stretch's ramp reached: all the way, unless the song jumped in the middle of it.
            const auto reached = grid.getReached();
            outputFrom = outputGain < 0.0f ? targetOutput : reached >= 1.0f ? outputGain : outputFrom + (outputGain - outputFrom) * reached;
            outputGain = isNear (outputFrom, targetOutput, 1.0e-5f) ? targetOutput : outputFrom + (targetOutput - outputFrom) * smoothing;
        }

        const auto length = grid.take (block.numSamples - start);
        const auto done = grid.getDone(), stretchLength = grid.getStepLength();

        if (numChannels > 0)
        {
            float* channels[2] = { block.channels[0] + start, block.channels[numChannels - 1] + start };

            // The dynamic bands hear the sound as it comes in, before any band.
            if (anyHearing)
            {
                for (int channel = 0; channel < numChannels; ++channel)
                    std::copy (channels[channel], channels[channel] + length, input[(size_t) channel].begin());

                for (auto& band : bands)
                    if (band.hearing)
                        hear (band, numChannels, length);
            }

            for (auto& band : bands)
                play (band, numChannels, channels, length);
        }

        // The output gain, ramped across the stretch.
        for (int channel = 0; channel < numChannels; ++channel)
        {
            auto* data = block.channels[channel] + start;

            for (int i = 0; i < length; ++i)
                data[i] *= outputFrom + (outputGain - outputFrom) * (float) (done + i + 1) / (float) stretchLength;
        }

        grid.advance (length);
        start += length;
    }

    // How far each band's Dynamic moves its gain now, for its editor.
    for (size_t index = 0; index < bands.size(); ++index)
    {
        const auto& band = bands[index];
        gainsMoved[index].set (band.on && isShelfOrBell (band.type) ? band.played - band.gain : 0.0f);
    }

    if (numChannels > 0)
        outputTap.push (block.channels[0], block.channels[numChannels - 1], block.numSamples);
}
} // namespace phazite::devices
