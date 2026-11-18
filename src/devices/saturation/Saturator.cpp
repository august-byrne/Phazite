// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Saturator.h"
#include "devices/dsp/Math.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace phazite::devices
{
namespace
{
    constexpr std::string_view shapes[] = { "Soft", "Tape", "Tube", "Hard", "Fold", "Crush" };
    constexpr std::string_view qualities[] = { "2x", "4x", "8x" };

    constexpr float lowestFrom = 20.0f, highestTo = 20000.0f; // From's lowest and To's highest: no edge there

    // The IDs are saved in projects.
    constexpr ParameterInfo parameters[] = {
        { .id = "drive", .name = "Drive", .minimum = -24.0f, .maximum = 36.0f, .defaultValue = 6.0f, .unit = Unit::decibels },
        { .id = "shape",
          .name = "Shape",
          .maximum = (float) std::size (shapes) - 1.0f,
          .defaultValue = 0.0f,
          .unit = Unit::choice,
          .choices = shapes },
        { .id = "colour", .name = "Colour", .defaultValue = 0.0f, .unit = Unit::percent },
        { .id = "tone", .name = "Tone", .minimum = -1.0f, .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::percent },
        { .id = "output", .name = "Output", .minimum = -24.0f, .maximum = 12.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
        { .id = "autoGain", .name = "Auto Gain", .maximum = 1.0f, .defaultValue = 1.0f, .unit = Unit::toggle },
        { .id = "band", .name = "Band", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::toggle },
        { .id = "bandLow",
          .name = "From",
          .minimum = lowestFrom,
          .maximum = 10000.0f,
          .defaultValue = 3000.0f,
          .unit = Unit::hertz,
          .scale = Scale::logarithmic },
        { .id = "bandHigh",
          .name = "To",
          .minimum = 200.0f,
          .maximum = highestTo,
          .defaultValue = highestTo,
          .unit = Unit::hertz,
          .scale = Scale::logarithmic },
        { .id = "quality",
          .name = "Quality",
          .maximum = (float) std::size (qualities) - 1.0f,
          .defaultValue = 0.0f,
          .unit = Unit::choice,
          .choices = qualities },
        { .id = mixParameterID, .name = "Mix", .defaultValue = 1.0f, .unit = Unit::percent },
    };

    constexpr double glideSeconds = 0.01;       // each of a glide's two steps (the drive, Auto Gain, the colour, the output, the
                                                // tone): about 20 ms in all
    constexpr double tiltFrequency = 1000.0;    // where the tone tilts around
    constexpr float tiltDecibels = 6.0f;        // how far the tone tilts each end, at its ends
    constexpr double dcBlockerFrequency = 10.0; // under what a lopsided curve leaves
    constexpr double fadeSeconds = 0.01;        // a change of Quality or of the Band switch, and an edge's fade
    constexpr double shapeFadeSeconds = 0.03;   // a change of shape: slow enough that two shapes that play a low tone
                                                // differently fade without a thump under it (10 ms made one, a 100 Hz
                                                // tone's difference spread down under 30 Hz), and heard whole in 35 ms
    constexpr double dcFadeSeconds = 0.005;     // the DC blocker coming in or going
    constexpr double listenFadeSeconds = 0.005; // between the sound and the band alone, as Listen turns on or off

    // How long a shape or a Quality plays unheard, from silence, before it is faded to: long enough for the curve's averaging
    // and the filters to the curve's rate to settle. Neither starts a DC blocker from rest under the sound: a shape takes
    // the one playing's (Path::startJoining()), and a Quality the one playing's when it has warmed up. The band's
    // crossovers, started from rest on a new Quality, take as long as their lowest takes (bandSettlingCycles of it).
    constexpr double warmSeconds = 0.005;
    // A joining DC blocker's glide, from 200 Hz to the DC blocker's 10 Hz in 100 ms, slower as it goes lower (one over its
    // corner rising in a straight line), so that each octave takes as many of its own cycles: slowly enough that what it
    // holds of a low tone follows it (in 30 ms a 100 Hz tone's difference between two shapes left a thump at -44 dBFS).
    constexpr double joinStartFrequency = 200.0, joinGlideSeconds = 0.1;
    constexpr double bandSettlingCycles = 1.6; // seven of a Butterworth pair's time constants: 61 dB down
    // An edge's glide: two octaves in about 13 ms, half as fast as Safe Bass's, so that a sound the band takes into the
    // curve or out of it as it moves goes from one way to the other no faster than a fade.
    constexpr double octavesPerSecond = 150.0;
    constexpr double butterworthQ = 0.70710678;
    constexpr float mostBias = 0.7f;      // how far Colour moves the curve's working point at 100 %
    constexpr float crushLevels = 128.0f; // Crush's steps a unit of sound at a drive of 0 dB: 256 across -1 to 1

    // The highest an edge fades in and out at, as a part of the sound's rate: above hearing, at twice the rate.
    constexpr double highOffRatio = 0.9;

    float getShapeValue (Saturator::Shape shape, float x) noexcept
    {
        switch (shape)
        {
            case Saturator::Shape::tape:
                return dsp::TapeCurve::value (x);
            case Saturator::Shape::tube:
                return dsp::TubeCurve::value (x);
            case Saturator::Shape::hard:
                return dsp::HardCurve::value (x);
            case Saturator::Shape::fold:
                return dsp::FoldCurve::value (x);
            case Saturator::Shape::crush:
                return dsp::CrushCurve::value (x);
            case Saturator::Shape::soft:
                break;
        }

        return dsp::SoftCurve::value (x);
    }

    // A curve's slope at a point, by a central difference.
    float getSlope (Saturator::Shape shape, float x) noexcept
    {
        constexpr float h = 1.0e-3f;
        return (getShapeValue (shape, x + h) - getShapeValue (shape, x - h)) / (2.0f * h);
    }

    // How much steeper than at its middle the curve is played at its working point: so that a quiet sound keeps its level at
    // any colour.
    float getSteepness (Saturator::Shape shape, float bias) noexcept
    {
        if (shape == Saturator::Shape::crush)
            return 1.0f;

        return getSlope (shape, 0.0f) / std::max (1.0e-3f, getSlope (shape, bias));
    }

    // A slot's curve through its antiderivative, for the shape it holds.
    float playShape (Saturator::Shape shape, dsp::AntialiasedCurve& curve, float x) noexcept
    {
        switch (shape)
        {
            case Saturator::Shape::tape:
                return curve.process (x, dsp::TapeCurve {});
            case Saturator::Shape::tube:
                return curve.process (x, dsp::TubeCurve {});
            case Saturator::Shape::hard:
                return curve.process (x, dsp::HardCurve {});
            case Saturator::Shape::fold:
                return curve.process (x, dsp::FoldCurve {});
            case Saturator::Shape::crush:
                return curve.process (x, dsp::CrushCurve {});
            case Saturator::Shape::soft:
                break;
        }

        return curve.process (x, dsp::SoftCurve {});
    }

    Saturator::Shape getShape (float value) noexcept
    {
        return (Saturator::Shape) juce::jlimit (0, (int) std::size (shapes) - 1, (int) std::lround (value));
    }

    int getQualityIndex (float value) noexcept
    {
        return juce::jlimit (0, (int) std::size (qualities) - 1, (int) std::lround (value));
    }
} // namespace

const DeviceInfo Saturator::info { .typeID = typeID, .name = "Saturator", .kind = DeviceKind::effect, .parameters = parameters };

float Saturator::playCurve (Shape curveShape, float input, float driveDecibels, float colourValue) noexcept
{
    const auto gain = dsp::decibelsToGain (driveDecibels);

    if (curveShape == Shape::crush)
    {
        const auto step = gain / crushLevels;
        return step * std::round (input / step);
    }

    const auto point = mostBias * std::clamp (colourValue, 0.0f, 1.0f);
    return (getShapeValue (curveShape, input * gain + point) - getShapeValue (curveShape, point)) * getSteepness (curveShape, point);
}

Saturator::Band Saturator::getBand (std::span<const float> values) noexcept
{
    Band shown;
    shown.from = std::clamp (values[bandLow], lowestFrom, parameters[bandLow].maximum);
    shown.to = std::clamp (std::max (values[bandHigh], 2.0f * shown.from), parameters[bandHigh].minimum, highestTo);
    shown.hasLow = shown.from > lowestFrom + 0.5f;
    shown.hasHigh = shown.to < highestTo - 0.5f;
    return shown;
}

namespace
{
    // How late a Quality's way comes, in the sound's samples, at any rate: the filters to the curve's rate and back, whole
    // samples late, and the curve's averaging and levelling filter, 4 samples at its rate, made a whole number of the
    // sound's samples (at 8 times the rate, half of one: 4 more make it whole).
    int getOwnLatency (int q)
    {
        static const std::array<int, 3> latencies = []
        {
            std::array<int, 3> found {};

            for (int each = 0; each < (int) found.size(); ++each)
            {
                juce::dsp::Oversampling<float> filters (2, (size_t) each + 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
                                                        true, true);
                filters.initProcessing (1);
                const auto factor = 2 << each;
                const auto padding = (factor - dsp::AntialiasedCurve::delay % factor) % factor;
                found[(size_t) each] =
                    (int) std::lround (filters.getLatencyInSamples()) + (dsp::AntialiasedCurve::delay + padding) / factor;
            }

            return found;
        }();

        return latencies[(size_t) q];
    }
} // namespace

int Saturator::getLatency()
{
    auto latest = 0;

    for (int q = 0; q < numQualities; ++q)
        latest = std::max (latest, getOwnLatency (q));

    return latest;
}

void Saturator::Glide::moveTo (float target, float step, float closeEnough) noexcept
{
    middle += (target - middle) * step;
    value += (middle - value) * step;

    if (std::abs (target - value) < closeEnough && std::abs (target - middle) < closeEnough)
        snap (target);
}

float Saturator::getEased (float along) noexcept
{
    // At either end, as it is: most of the time nothing fades.
    if (along <= 0.0f)
        return 0.0f;

    if (along >= 1.0f)
        return 1.0f;

    return 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * along);
}

void Saturator::Controls::allocate (int length)
{
    for (auto* each : { &drive, &output, &bias, &slotFade, &bandShare, &lowShare, &highShare, &listen, &qualityFade, &tilt })
        each->assign ((size_t) length, 0.0f);

    for (auto* each : { &steepness, &back, &straightGain, &dcShare })
        for (auto& slotValues : *each)
            slotValues.assign ((size_t) length, 1.0f);
}

void Saturator::Edge::snap() noexcept
{
    amount = on ? 1.0f : 0.0f;
    frequency = target;
    starts = false;
}

void Saturator::Edge::move (float fade, float octaves) noexcept
{
    starts = false;

    if (on && amount < 1.0f)
    {
        // Fading in where it fades, from silence if it was out.
        if (amount <= 0.0f)
        {
            frequency = off;
            starts = true;
        }

        amount = std::min (1.0f, amount + fade);
    }
    else if (! juce::exactlyEqual (frequency, target))
    {
        // In octaves, toward its place, or to where it fades out.
        const auto away = std::log2 (target / frequency);
        frequency = std::abs (away) <= octaves ? target : frequency * std::exp2 (away > 0.0f ? octaves : -octaves);
    }
    else if (! on && amount > 0.0f)
    {
        amount = std::max (0.0f, amount - fade);
    }
}

void Saturator::Path::reset() noexcept
{
    for (auto* each : { &filters, &dryFilters })
        if (*each != nullptr)
            (*each)->reset();

    for (auto& channel : channels)
    {
        for (auto& curve : channel.curves)
            curve.reset();

        for (auto* curve : { &channel.straight, &channel.outside, &channel.dryStraight })
            curve->reset();

        for (auto* crossover : { &channel.low, &channel.high, &channel.dryLow, &channel.dryHigh })
            crossover->reset();

        for (auto* filter : { &channel.bentHighPass, &channel.lowTilt, &channel.highTilt })
            filter->reset();

        for (auto* filter : { &channel.offsets[0], &channel.offsets[1], &channel.joining })
            filter->reset();

        channel.padded.fill (0.0f);
        channel.dryPadded.fill (0.0f);
        channel.late.reset();
        channel.dryLate.reset();
    }

    for (auto& point : points)
        point.reset();

    pointLevels.fill (0.0f);
    restedFor.fill (2 * dsp::AntialiasedCurve::delay);
    paddedAt = 0;
    lastLow = lastHigh = -1.0f;
    lastTilt = -2.0f;
    lastBias = 0.0f;
    dryInStep = false;
    joins = false;
}

void Saturator::Path::setJoinCorner (float hertz) noexcept
{
    joinCorner = hertz;
    joinFilter = dsp::makeFilter (dsp::FilterShape::highPass, hertz, rate, butterworthQ);
}

void Saturator::Path::startJoining() noexcept
{
    joins = true;
    setJoinCorner ((float) joinStartFrequency);

    for (auto& channel : channels)
        channel.joining.reset();
}

void Saturator::Path::save (OffsetStates& states) const noexcept
{
    for (size_t c = 0; c < channels.size(); ++c)
    {
        states.offsets[c] = channels[c].offsets;
        states.joining[c] = channels[c].joining;
    }

    states.joinCorner = joinCorner;
    states.joins = joins;
}

void Saturator::Path::load (const OffsetStates& states) noexcept
{
    for (size_t c = 0; c < channels.size(); ++c)
    {
        channels[c].offsets = states.offsets[c];
        channels[c].joining = states.joining[c];
    }

    joins = states.joins;

    if (joins)
        setJoinCorner (states.joinCorner);
}

void Saturator::Path::startSlot (int index, Shape slotShape, float pointValue) noexcept
{
    const auto s = (size_t) index;
    points[s].reset();
    pointLevels[s] = 0.0f;
    restedFor[s] = 2 * dsp::AntialiasedCurve::delay;
    lastBias = pointValue;

    for (auto& channel : channels)
    {
        channel.curves[s].reset();
        channel.offsets[s].reset();
    }

    // Off its middle, the curve and the one beside it that plays its working point alone start as if they had rested there,
    // so that the first samples aren't the way up to it.
    if (slotShape == Shape::crush || juce::exactlyEqual (pointValue, 0.0f))
        return;

    for (int i = 0; i < 2 * dsp::AntialiasedCurve::delay; ++i)
    {
        pointLevels[s] = playShape (slotShape, points[s], pointValue);

        for (auto& channel : channels)
            playShape (slotShape, channel.curves[s], pointValue);
    }
}

void Saturator::Path::startSplit (const Step& now) noexcept
{
    for (auto& channel : channels)
    {
        for (auto* curve : { &channel.straight, &channel.outside })
            curve->reset();

        for (auto* crossover : { &channel.low, &channel.high, &channel.dryLow, &channel.dryHigh })
            crossover->reset();

        channel.bentHighPass.reset();
    }

    setStep (now);
}

void Saturator::Path::setStep (const Step& now) noexcept
{
    if (! juce::exactlyEqual (now.low, lastLow))
    {
        lastLow = now.low;
        channels[0].low.setFrequency (now.low, rate);
        highPass = dsp::makeFilter (dsp::FilterShape::highPass, now.low, rate, butterworthQ);

        for (auto& channel : channels)
        {
            channel.low.setFrequencyAs (channels[0].low);
            channel.dryLow.setFrequencyAs (channels[0].low);
        }
    }

    if (! juce::exactlyEqual (now.high, lastHigh))
    {
        lastHigh = now.high;
        channels[0].high.setFrequency (now.high, rate);

        for (auto& channel : channels)
        {
            channel.high.setFrequencyAs (channels[0].high);
            channel.dryHigh.setFrequencyAs (channels[0].high);
        }
    }

    // An edge coming in from nothing: its filters start from silence.
    for (auto& channel : channels)
    {
        if (now.lowStarts)
        {
            channel.low.reset();
            channel.dryLow.reset();
            channel.bentHighPass.reset();
        }

        if (now.highStarts)
        {
            channel.high.reset();
            channel.dryHigh.reset();
        }
    }
}

void Saturator::Path::setTilt (float now) noexcept
{
    if (juce::exactlyEqual (now, lastTilt))
        return;

    lastTilt = now;
    lowTilt = dsp::makeFilter (dsp::FilterShape::lowShelf, tiltFrequency, rate, 0.5, -tiltDecibels * now);
    highTilt = dsp::makeFilter (dsp::FilterShape::highShelf, tiltFrequency, rate, 0.5, tiltDecibels * now);
}

void Saturator::prepare (double newSampleRate, int maximumBlockSize)
{
    sampleRate = newSampleRate;
    capacity = std::max (1, maximumBlockSize);
    dsp::TubeCurve::prepare();

    // Every Quality made ready at once, so that a change of it while the song plays allocates nothing: filters to 2, 4 or 8
    // times the rate and back, polyphase half-band ones that keep the latency short and whole.
    for (int q = 0; q < numQualities; ++q)
    {
        auto& path = paths[(size_t) q];
        path.factor = 2 << q;
        path.rate = sampleRate * path.factor;
        path.padding = (path.factor - dsp::AntialiasedCurve::delay % path.factor) % path.factor;
        path.ownLatency = getOwnLatency (q);

        path.offsetFilter = dsp::makeFilter (dsp::FilterShape::highPass, dcBlockerFrequency, path.rate, butterworthQ);
        path.setJoinCorner ((float) dcBlockerFrequency);

        for (auto* filters : { &path.filters, &path.dryFilters })
        {
            *filters = std::make_unique<juce::dsp::Oversampling<float>> (
                2, (size_t) q + 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);
            (*filters)->initProcessing ((size_t) capacity);
        }
    }

    // Every Quality as late as the latest, which the others wait for (DelayLine late, sized for it).
    latency = getLatency();

    for (auto& path : paths)
    {
        path.delay = std::max (0, latency - path.ownLatency);

        for (auto& channel : path.channels)
        {
            channel.late.prepare (path.delay + 1);
            channel.dryLate.prepare (path.delay + 1);
        }
    }

    smoothing = (float) (1.0 - std::exp (-1.0 / (glideSeconds * sampleRate)));
    fadePerSample = (float) (1.0 / (fadeSeconds * sampleRate));
    edgeFadePerSample = fadePerSample;
    shapeFadePerSample = (float) (1.0 / (shapeFadeSeconds * sampleRate));
    octavesPerSample = (float) (octavesPerSecond / sampleRate);
    listenStep = (float) (1.0 / (listenFadeSeconds * sampleRate));
    dcStep = (float) (1.0 / (dcFadeSeconds * sampleRate));
    joinGlide = (float) ((1.0 / dcBlockerFrequency - 1.0 / joinStartFrequency) / (joinGlideSeconds * sampleRate));
    warmUp = std::max (256, (int) std::lround (warmSeconds * sampleRate));
    low.off = 5.0f;
    high.off = (float) (highOffRatio * sampleRate);

    controls.allocate (capacity);
    steps.assign ((size_t) capacity + 1, Step {});
    spare.setSize (2, capacity);
    spareDry.setSize (2, capacity);
    reset();
}

void Saturator::reset()
{
    for (auto& path : paths)
        path.reset();

    active = 0;
    next = -1;
    warmed = 0;
    qualityFade = 0.0f;
    slotFading = false;
    slotFade = 0.0f;
    slotWarmed = 0;
    dcShares.fill (0.0f);
    bandShare = 0.0f;
    splitting = false;
    listenShare = 0.0f;
    lastMix = 1.0f;
    isFirstBlock = true;
}

bool Saturator::isLopsided (int index, float targetBias) const noexcept
{
    const auto slotShape = slotShapes[(size_t) index];
    return slotShape == Shape::tube || (slotShape != Shape::crush && (bias.value > 0.0f || targetBias > 0.0f));
}

void Saturator::updateSlopes (int index, float atBias) noexcept
{
    const auto s = (size_t) index;
    const auto slotShape = slotShapes[s];

    if (slotShape == slopedShapes[s] && juce::exactlyEqual (atBias, slopedAt[s]))
        return;

    if (slotShape != slopedShapes[s])
        middleSlopes[s] = slotShape == Shape::crush ? 1.0f : getSlope (slotShape, 0.0f);

    slopedShapes[s] = slotShape;
    slopedAt[s] = atBias;
    steepnesses[s] = getSteepness (slotShape, atBias);
}

int Saturator::getBandSettling() const noexcept
{
    if (! splitting)
        return 0;

    auto lowest = std::numeric_limits<float>::max();

    for (const auto* edge : { &low, &high })
        if (edge->on || edge->amount > 0.0f)
            lowest = std::min (lowest, std::max (edge->frequency, lowestFrom)); // one fading out of hearing as From's lowest

    return lowest < std::numeric_limits<float>::max() ? (int) std::lround (bandSettlingCycles / lowest * sampleRate) : 0;
}

void Saturator::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    silenceWhatIsNotSound (block);
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);

    if (paths[0].filters == nullptr || numChannels <= 0 || block.numSamples <= 0)
        return;

    // What came in, for the mix: only while the mix uses it (it starts from silence then, as the mix leaves 100 %); with the
    // mix all the saturator's own sound, nothing reads it.
    const auto usesDry = block.dry != nullptr && (values[mix] < 1.0f || lastMix < 1.0f);
    lastMix = values[mix];

    // In stretches as long as the block it was prepared for, at most.
    for (int done = 0; done < block.numSamples;)
    {
        const auto length = std::min (capacity, block.numSamples - done);
        float* channels[2] = { block.channels[0] + done, numChannels > 1 ? block.channels[1] + done : nullptr };
        float* dry[2] = { nullptr, nullptr };

        if (usesDry)
        {
            dry[0] = block.dry[0] + done;
            dry[1] = numChannels > 1 ? block.dry[1] + done : nullptr;
        }

        play (channels, dry, numChannels, length, values, usesDry);
        done += length;
    }
}

void Saturator::play (float* const* channels, float* const* dry, int numChannels, int length, std::span<const float> values, bool usesDry)
{
    const auto wantedShape = getShape (values[shape]);
    const auto targetBias = wantedShape == Shape::crush ? 0.0f : mostBias * std::clamp (values[colour], 0.0f, 1.0f);
    const auto wantsBand = values[band] > 0.5f;
    const auto edges = getBand (values);
    low.on = edges.hasLow;
    low.target = edges.hasLow ? edges.from : low.off;
    high.on = edges.hasHigh;
    high.target = edges.hasHigh ? edges.to : high.off;
    const auto current = [this]
    {
        Step now;
        now.low = low.frequency;
        now.high = high.frequency;
        return now;
    };

    if (isFirstBlock)
    {
        isFirstBlock = false;
        driveDecibels.snap (values[drive]);
        driveGain = dsp::decibelsToGain (driveDecibels.value);
        autoShare.snap (values[autoGain] > 0.5f ? 1.0f : 0.0f);
        outputGain.snap (dsp::decibelsToGain (values[output]));
        bias.snap (targetBias);
        tilt.snap (std::clamp (values[tone], -1.0f, 1.0f));
        slotShapes[(size_t) slot] = wantedShape;
        dcShares[(size_t) slot] = isLopsided (slot, targetBias) ? 1.0f : 0.0f;
        bandShare = wantsBand ? 1.0f : 0.0f;
        splitting = wantsBand;
        listenShare = 0.0f;
        low.snap();
        high.snap();
        active = getQualityIndex (values[quality]);
        auto& path = paths[(size_t) active];
        path.reset();
        path.startSlot (slot, wantedShape, bias.value);
        path.setStep (current());
        path.setTilt (tilt.value);

        if (splitting)
            path.startSplit (current());
    }

    // While the band's filters are out, its edges are where they are set, ready for it.
    if (! splitting)
    {
        low.snap();
        high.snap();
    }

    // A path that plays, and one warming up beside it.
    const auto forEachPlaying = [this] (auto&& action)
    {
        action (paths[(size_t) active]);

        if (next >= 0)
            action (paths[(size_t) next]);
    };

    // Another shape: it starts in the other slot, its DC blocker joining the one playing's (Path::startJoining()), plays
    // unheard while its curve settles, and the sound fades to it.
    if (! slotFading && wantedShape != slotShapes[(size_t) slot])
    {
        const auto other = 1 - slot;
        slotShapes[(size_t) other] = wantedShape;
        slotFading = true;
        slotFade = 0.0f;
        slotWarmed = 0;
        dcShares[(size_t) other] = isLopsided (other, targetBias) ? 1.0f : 0.0f;
        forEachPlaying (
            [&] (Path& path)
            {
                path.startSlot (other, wantedShape, bias.value);
                path.startJoining();
            });
    }

    // The band switched on: its filters start, and the sound fades to it.
    if (wantsBand && ! splitting)
    {
        splitting = true;
        forEachPlaying ([&] (Path& path) { path.startSplit (current()); });
    }

    // Another Quality: it starts beside the one playing, plays unheard while its filters settle (the band's, from rest, as
    // long as the lowest takes), takes the DC blockers of the one playing as they are then, and the sound fades to it.
    if (const auto wanted = getQualityIndex (values[quality]); next < 0 && wanted != active)
    {
        next = wanted;
        warmed = 0;
        qualityFade = 0.0f;
        qualityWarmUp = std::max (warmUp, getBandSettling());
        auto& path = paths[(size_t) next];
        path.reset();
        path.startSlot (slot, slotShapes[(size_t) slot], bias.value);

        if (slotFading)
            path.startSlot (1 - slot, slotShapes[(size_t) (1 - slot)], bias.value);

        path.setStep (current());
        path.setTilt (tilt.value);

        if (splitting)
            path.startSplit (current());
    }

    workOutControls (length, values);

    // What came in, for the one warming up.
    if (next >= 0)
    {
        for (int channel = 0; channel < numChannels; ++channel)
        {
            spare.copyFrom (channel, 0, channels[channel], length);

            if (usesDry)
                spareDry.copyFrom (channel, 0, dry[channel], length);
        }
    }

    playPath (paths[(size_t) active], channels, dry, numChannels, length, usesDry);

    if (next >= 0)
    {
        auto* const* nextChannels = spare.getArrayOfWritePointers();
        auto* const* nextDry = spareDry.getArrayOfWritePointers();
        playPath (paths[(size_t) next], nextChannels, nextDry, numChannels, length, usesDry);

        for (int channel = 0; channel < numChannels; ++channel)
        {
            for (int i = 0; i < length; ++i)
            {
                const auto q = controls.qualityFade[(size_t) i];
                channels[channel][i] += q * (nextChannels[channel][i] - channels[channel][i]);

                if (usesDry)
                    dry[channel][i] += q * (nextDry[channel][i] - dry[channel][i]);
            }
        }
    }

    // What has finished fading (and a new shape's DC blocker joining, which needs the old shape played until then).
    if (slotFading && slotFade >= 1.0f && ! paths[(size_t) active].joins && (next < 0 || ! paths[(size_t) next].joins))
    {
        dcShares[(size_t) slot] = 0.0f;
        slot = 1 - slot;
        slotFading = false;
        slotFade = 0.0f;
    }

    if (next >= 0 && qualityFade >= 1.0f)
    {
        active = next;
        next = -1;
        qualityFade = 0.0f;
    }

    if (splitting && ! wantsBand && bandShare <= 0.0f)
        splitting = false;
}

void Saturator::workOutControls (int length, std::span<const float> values)
{
    const auto wantedShape = getShape (values[shape]);
    const auto targetDecibels = values[drive];
    const auto targetAuto = values[autoGain] > 0.5f ? 1.0f : 0.0f;
    const auto targetOutput = dsp::decibelsToGain (values[output]);
    const auto targetBias = wantedShape == Shape::crush ? 0.0f : mostBias * std::clamp (values[colour], 0.0f, 1.0f);
    const auto targetTilt = std::clamp (values[tone], -1.0f, 1.0f);
    const auto wantedBand = values[band] > 0.5f ? 1.0f : 0.0f;
    const auto wantedListen = isPlayingBand() ? 1.0f : 0.0f;
    const std::array<bool, 2> lopsided { isLopsided (0, targetBias), isLopsided (1, targetBias) };
    numSteps = 0;
    handOverAt = -1;

    for (int i = 0; i < length; ++i)
    {
        // The band's edges move on a sample: where they moved, the filters at them are worked out again.
        if (splitting)
        {
            low.move (edgeFadePerSample, octavesPerSample);
            high.move (edgeFadePerSample, octavesPerSample);
        }

        if (numSteps == 0 || low.starts || high.starts || ! juce::exactlyEqual (low.frequency, steps[(size_t) numSteps - 1].low)
            || ! juce::exactlyEqual (high.frequency, steps[(size_t) numSteps - 1].high))
        {
            auto& now = steps[(size_t) numSteps++];
            now.at = i;
            now.low = low.frequency;
            now.high = high.frequency;
            now.lowStarts = splitting && low.starts;
            now.highStarts = splitting && high.starts;
        }

        {
            const auto at = (size_t) i;

            if (! driveDecibels.isAt (targetDecibels))
            {
                driveDecibels.moveTo (targetDecibels, smoothing, 1.0e-4f);
                driveGain = dsp::decibelsToGain (driveDecibels.value);
            }

            if (! autoShare.isAt (targetAuto))
                autoShare.moveTo (targetAuto, smoothing, 1.0e-6f);

            if (! outputGain.isAt (targetOutput))
                outputGain.moveTo (targetOutput, smoothing, 1.0e-7f);

            if (! bias.isAt (targetBias))
                bias.moveTo (targetBias, smoothing, 1.0e-6f);

            if (! tilt.isAt (targetTilt))
                tilt.moveTo (targetTilt, smoothing, 1.0e-5f);

            if (slotFading)
            {
                if (slotWarmed < warmUp)
                    ++slotWarmed;
                else
                    slotFade = std::min (1.0f, slotFade + shapeFadePerSample);
            }

            bandShare += std::clamp (wantedBand - bandShare, -fadePerSample, fadePerSample);
            listenShare += std::clamp (wantedListen - listenShare, -listenStep, listenStep);

            if (next >= 0)
            {
                if (warmed < qualityWarmUp)
                {
                    if (++warmed == qualityWarmUp)
                        handOverAt = i;
                }
                else
                    qualityFade = std::min (1.0f, qualityFade + fadePerSample);
            }

            controls.drive[at] = driveGain;
            controls.output[at] = outputGain.value;
            controls.bias[at] = bias.value;
            controls.tilt[at] = tilt.value;
            controls.slotFade[at] = slotFading ? getEased (slotFade) : 0.0f;
            controls.bandShare[at] = getEased (bandShare);
            controls.lowShare[at] = getEased (low.amount);
            controls.highShare[at] = getEased (high.amount);
            controls.listen[at] = getEased (listenShare);
            controls.qualityFade[at] = next >= 0 ? getEased (qualityFade) : 0.0f;

            // Auto Gain takes the drive back off: quiet parts, which the curve leaves straight, keep their level.
            const auto back = 1.0f - autoShare.value + autoShare.value / driveGain;

            for (int s = 0; s < 2; ++s)
            {
                const auto index = (size_t) s;
                auto& dcShare = dcShares[index];
                dcShare += std::clamp ((lopsided[index] ? 1.0f : 0.0f) - dcShare, -dcStep, dcStep);
                controls.dcShare[index][at] = getEased (dcShare);

                if (slotShapes[index] == Shape::crush)
                {
                    controls.steepness[index][at] = 1.0f;
                    controls.back[index][at] = 1.0f;
                    controls.straightGain[index][at] = 1.0f;
                    continue;
                }

                // Worked out again only as the working point or the shape moves.
                updateSlopes (s, bias.value);
                controls.steepness[index][at] = steepnesses[index];
                controls.back[index][at] = back;
                controls.straightGain[index][at] = driveGain * middleSlopes[index] * back;
            }
        }
    }
}

void Saturator::playPath (Path& path, float* const* channels, float* const* dry, int numChannels, int length, bool usesDry)
{
    juce::dsp::AudioBlock<float> audio (channels, (size_t) numChannels, (size_t) length);
    auto fast = path.filters->processSamplesUp (audio);

    // The copy of what came in, for the mix: through the same filters, as late as the sound and its phase turned alike, a
    // straight line in the curve's place and the band's all-passes in its split's.
    if (usesDry && ! path.dryInStep)
    {
        path.dryFilters->reset();

        for (auto& channel : path.channels)
        {
            channel.dryStraight.reset();
            channel.dryLow.reset();
            channel.dryHigh.reset();
            channel.dryPadded.fill (0.0f);
            channel.dryLate.reset();
        }
    }

    juce::dsp::AudioBlock<float> came;
    juce::dsp::AudioBlock<float> fastDry;

    if (usesDry)
    {
        came = juce::dsp::AudioBlock<float> (dry, (size_t) numChannels, (size_t) length);
        fastDry = path.dryFilters->processSamplesUp (came);
    }

    const auto factor = path.factor;
    const auto playing = slot, other = 1 - slot;
    const auto playingShape = slotShapes[(size_t) playing], otherShape = slotShapes[(size_t) other];
    const auto both = slotFading;
    const auto split = splitting;
    auto stepIndex = 0;

    for (int i = 0; i < length; ++i)
    {
        while (stepIndex < numSteps && steps[(size_t) stepIndex].at == i)
            path.setStep (steps[(size_t) stepIndex++]);

        const auto at = (size_t) i;
        path.setTilt (controls.tilt[at]);
        const auto gain = controls.drive[at], out = controls.output[at], point = controls.bias[at];
        const auto fade = controls.slotFade[at], share = controls.bandShare[at];
        const auto lowShare = controls.lowShare[at], highShare = controls.highShare[at], heard = 1.0f - controls.listen[at];
        const auto straightGain = controls.straightGain[(size_t) playing][at]
                                  + fade * (controls.straightGain[(size_t) other][at] - controls.straightGain[(size_t) playing][at]);
        const auto highPassShare = share * lowShare;

        // The working point glides where Colour puts it: what the curve makes of it alone is taken off what it makes of the
        // sound there, played the same way beside it while the point moves, so that it is as late and as smooth.
        if (! juce::exactlyEqual (point, path.lastBias))
        {
            path.lastBias = point;
            path.restedFor.fill (0);
        }

        // A Quality warmed up beside the one playing takes its DC blockers as they are here, settled on the sound.
        if (i == handOverAt)
        {
            if (&path == &paths[(size_t) active])
                path.save (handedOver);
            else
                path.load (handedOver);
        }

        // A shape's DC blocker joining the one playing glides down to its corner; there the two become the new shape's own.
        if (path.joins)
        {
            if (! both)
            {
                path.joins = false;
            }
            else if (path.joinCorner <= (float) dcBlockerFrequency)
            {
                // The same filter now, and a linear one: the sum of what the two hold is what one fed the sum holds.
                for (auto& channel : path.channels)
                {
                    channel.offsets[(size_t) other].ic1 = channel.offsets[(size_t) playing].ic1 + channel.joining.ic1;
                    channel.offsets[(size_t) other].ic2 = channel.offsets[(size_t) playing].ic2 + channel.joining.ic2;
                }

                path.joins = false;
            }
            else
            {
                const auto lower = 1.0f / (1.0f / path.joinCorner + joinGlide);
                path.setJoinCorner (lower < (float) dcBlockerFrequency + 0.01f ? (float) dcBlockerFrequency : lower);
            }
        }

        const auto joining = path.joins;

        for (int k = 0; k < factor; ++k)
        {
            const auto j = (size_t) (i * factor + k);

            for (int s = 0; s < 2; ++s)
            {
                const auto slotShape = slotShapes[(size_t) s];

                if ((s == other && ! both) || slotShape == Shape::crush || path.restedFor[(size_t) s] >= 2 * dsp::AntialiasedCurve::delay)
                    continue;

                path.pointLevels[(size_t) s] = playShape (slotShape, path.points[(size_t) s], point);
                ++path.restedFor[(size_t) s];
            }

            ++path.paddedAt;

            for (int c = 0; c < numChannels; ++c)
            {
                auto& state = path.channels[(size_t) c];
                auto* samples = fast.getChannelPointer ((size_t) c);
                const auto x = samples[j];
                auto into = x, outside = 0.0f;

                // The band, split off at its edges: what is above From and below To goes into the curve, and what is outside
                // it waits beside the curve as a straight line, turned by the other crossover's all-pass as the band is.
                if (split)
                {
                    auto above = x, below = 0.0f;

                    if (lowShare > 0.0f)
                    {
                        const auto bands = state.low.process (x);
                        above = x + lowShare * (bands.high - x);
                        below = lowShare * bands.low;
                    }

                    auto inBand = above, top = 0.0f;

                    if (highShare > 0.0f)
                    {
                        const auto bands = state.high.process (above);
                        inBand = above + highShare * (bands.low - above);
                        top = highShare * bands.high;
                        below += highShare * (state.high.processAllPass (below) - below);
                    }

                    into = x + share * (inBand - x);
                    outside = share * (below + top);
                }

                // Into the curve, around its working point, or rounded to Crush's steps; its offset taken off where it is
                // lopsided; the drive taken back off as Auto Gain says; faded from one shape to another. The DC blocker
                // plays all along, so that it has settled on the sound when the curve becomes lopsided and the sound fades
                // to its way; while the curve is even, the sound goes round it, as it came.
                const auto playCurveOf = [&] (int s, Shape slotShape)
                {
                    const auto index = (size_t) s;
                    auto& curve = state.curves[index];

                    if (slotShape == Shape::crush)
                    {
                        const auto step = gain / crushLevels;
                        return playShape (slotShape, curve, into / step) * step;
                    }

                    return (playShape (slotShape, curve, into * gain + point) - path.pointLevels[index]) * controls.steepness[index][at];
                };

                const auto finish = [&] (int s, float y, float blockedY)
                {
                    const auto index = (size_t) s;

                    if (const auto blocked = controls.dcShare[index][at]; blocked > 0.0f)
                        y += blocked * (blockedY - y);

                    return y * controls.back[index][at];
                };

                const auto playingY = playCurveOf (playing, playingShape);
                const auto playingBlocked = state.offsets[(size_t) playing].process (playingY, path.offsetFilter);
                auto played = finish (playing, playingY, playingBlocked);

                if (both)
                {
                    // A shape joining: the one playing's DC blocker, and another on what it plays differently.
                    const auto otherY = playCurveOf (other, otherShape);
                    const auto otherBlocked = joining ? playingBlocked + state.joining.process (otherY - playingY, path.joinFilter)
                                                      : state.offsets[(size_t) other].process (otherY, path.offsetFilter);
                    const auto otherPlayed = finish (other, otherY, otherBlocked);
                    played += fade * (otherPlayed - played);
                }

                auto sound = played, around = 0.0f;

                // What the curve bent of the band (what it played, less the band as a straight line would play it) through a
                // high-pass at From, so that what it makes under the band stays out; the band itself as it came.
                if (split)
                {
                    const auto straight = state.straight.process (into, dsp::StraightCurve {}) * straightGain;
                    auto bent = played - straight;

                    if (highPassShare > 0.0f)
                        bent += highPassShare * (state.bentHighPass.process (bent, path.highPass) - bent);

                    sound = straight + bent;
                    around = state.outside.process (outside, dsp::StraightCurve {});
                }

                sound = state.highTilt.process (state.lowTilt.process (sound, path.lowTilt), path.highTilt);
                auto y = sound * out + heard * around;

                // As many samples later as make its way a whole number of the sound's samples.
                if (path.padding > 0)
                {
                    state.padded[path.paddedAt & 7u] = y;
                    y = state.padded[(path.paddedAt - (unsigned) path.padding) & 7u];
                }

                samples[j] = y;

                if (usesDry)
                {
                    auto* drySamples = fastDry.getChannelPointer ((size_t) c);
                    auto v = drySamples[j];

                    if (split)
                    {
                        auto turned = v;

                        if (lowShare > 0.0f)
                            turned += lowShare * (state.dryLow.processAllPass (turned) - turned);

                        if (highShare > 0.0f)
                            turned += highShare * (state.dryHigh.processAllPass (turned) - turned);

                        v += share * (turned - v);
                    }

                    v = state.dryStraight.process (v, dsp::StraightCurve {});

                    if (path.padding > 0)
                    {
                        state.dryPadded[path.paddedAt & 7u] = v;
                        v = state.dryPadded[(path.paddedAt - (unsigned) path.padding) & 7u];
                    }

                    drySamples[j] = v;
                }
            }
        }
    }

    path.filters->processSamplesDown (audio);

    if (usesDry)
        path.dryFilters->processSamplesDown (came);

    path.dryInStep = usesDry;

    // As late as the latency set by hand, where this Quality comes sooner.
    if (path.delay > 0)
    {
        for (int c = 0; c < numChannels; ++c)
        {
            auto& state = path.channels[(size_t) c];

            for (int i = 0; i < length; ++i)
            {
                state.late.push (channels[c][i]);
                channels[c][i] = state.late.read (path.delay);

                if (usesDry)
                {
                    state.dryLate.push (dry[c][i]);
                    dry[c][i] = state.dryLate.read (path.delay);
                }
            }
        }
    }
}
} // namespace phazite::devices
