// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Multiband.h"
#include "devices/dsp/Math.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
namespace
{
    constexpr std::string_view slopes[] = { "12 dB", "24 dB" };
    constexpr std::string_view modes[] = { "On", "Comp Off", "Muted", "Off" };
    constexpr std::string_view detections[] = { "Peak", "RMS" };
    constexpr std::string_view curveShapes[] = { "1", "2", "3", "4", "5", "6", "7", "8" };
    constexpr std::string_view pointTypes[] = { "Single Curve", "Double Curve", "Hold", "Stairs", "Smooth Stairs" };

    constexpr ParameterInfo makeChoice (std::string_view id, std::string_view name, std::span<const std::string_view> choices,
                                        float defaultValue)
    {
        return { .id = id,
                 .name = name,
                 .minimum = 0.0f,
                 .maximum = (float) choices.size() - 1.0f,
                 .defaultValue = defaultValue,
                 .unit = Unit::choice,
                 .choices = choices };
    }

    constexpr ParameterInfo makeTime (std::string_view id, std::string_view name, float maximum, float defaultValue, float midpoint)
    {
        return { .id = id,
                 .name = name,
                 .minimum = 0.0f,
                 .maximum = maximum,
                 .defaultValue = defaultValue,
                 .unit = Unit::milliseconds,
                 .midpoint = midpoint };
    }

    constexpr ParameterInfo makeDecibels (std::string_view id, std::string_view name, float minimum, float maximum)
    {
        return { .id = id, .name = name, .minimum = minimum, .maximum = maximum, .defaultValue = 0.0f, .unit = Unit::decibels };
    }

    constexpr ParameterInfo makeBipolar (std::string_view id, std::string_view name)
    {
        return { .id = id, .name = name, .minimum = -1.0f, .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::percent };
    }

    // A point of a band's curve, from its band's prefix: at first the curve is a limiter at 0 dB, as Maximus's default is: up
    // the diagonal to 0 dB (the middle), then level to the end.
#define PHAZITE_MULTIBAND_POINT(band, Band, n, x, y)                                                                                       \
    { .id = band "Point" #n "X", .name = Band " Point " #n " X", .defaultValue = x, .unit = Unit::percent },                               \
        { .id = band "Point" #n "Y", .name = Band " Point " #n " Y", .defaultValue = y, .unit = Unit::percent },                           \
        makeBipolar (band "Point" #n "Tension", Band " Point " #n " Tension"),                                                             \
        makeChoice (band "Point" #n "Type", Band " Point " #n " Type", pointTypes, 0.0f)

    // A band's parameters, in the order of Multiband::BandField. The master looks ahead by its attack, so it starts quicker.
#define PHAZITE_MULTIBAND_BAND(band, Band, attackDefault)                                                                                  \
    makeChoice (band "Mode", Band " Mode", modes, 0.0f),                                                                                   \
        { .id = band "Solo", .name = Band " Solo", .maximum = 1.0f, .unit = Unit::toggle }, makeBipolar (band "Stereo", Band " Stereo"),   \
        makeDecibels (band "Pre", Band " Pre", -24.0f, 24.0f), makeDecibels (band "Post", Band " Post", -24.0f, 24.0f),                    \
        makeTime (band "Attack", Band " Attack", 500.0f, attackDefault, 20.0f),                                                            \
        makeTime (band "Release", Band " Release", 2000.0f, 0.0f, 200.0f),                                                                 \
        makeTime (band "Release2", Band " Release 2", 2000.0f, 100.0f, 200.0f),                                                            \
        makeTime (band "Sustain", Band " Sustain", 1000.0f, 20.0f, 100.0f), makeChoice (band "Curve", Band " Curve", curveShapes, 1.0f),   \
        makeChoice (band "ReleaseCurve", Band " Release Curve", curveShapes, 2.0f),                                                        \
        makeChoice (band "Detection", Band " Detection", detections, 0.0f), makeBipolar (band "Saturation", Band " Saturation"),           \
        makeDecibels (band "Ceiling", Band " Ceiling", -24.0f, 6.0f),                                                                      \
        { .id = band "Points", .name = Band " Points", .minimum = 2.0f, .maximum = 8.0f, .defaultValue = 3.0f },                           \
        PHAZITE_MULTIBAND_POINT (band, Band, 1, 0.0f, 0.0f), PHAZITE_MULTIBAND_POINT (band, Band, 2, 0.5f, 0.5f),                          \
        PHAZITE_MULTIBAND_POINT (band, Band, 3, 1.0f, 0.5f), PHAZITE_MULTIBAND_POINT (band, Band, 4, 1.0f, 0.5f),                          \
        PHAZITE_MULTIBAND_POINT (band, Band, 5, 1.0f, 0.5f), PHAZITE_MULTIBAND_POINT (band, Band, 6, 1.0f, 0.5f),                          \
        PHAZITE_MULTIBAND_POINT (band, Band, 7, 1.0f, 0.5f), PHAZITE_MULTIBAND_POINT (band, Band, 8, 1.0f, 0.5f)

    // The IDs are saved in projects.
    constexpr ParameterInfo parameters[] = {
        { .id = "lowCut",
          .name = "Low Cut",
          .minimum = 10.0f,
          .maximum = 500.0f,
          .defaultValue = 20.0f,
          .unit = Unit::hertz,
          .midpoint = 60.0f },
        { .id = "lowFrequency",
          .name = "Low Frequency",
          .minimum = 20.0f,
          .maximum = 2000.0f,
          .defaultValue = 120.0f,
          .unit = Unit::hertz,
          .midpoint = 200.0f },
        { .id = "highFrequency",
          .name = "High Frequency",
          .minimum = 500.0f,
          .maximum = 20000.0f,
          .defaultValue = 5000.0f,
          .unit = Unit::hertz,
          .midpoint = 3000.0f },
        makeChoice ("lowSlope", "Low Slope", slopes, 1.0f),
        makeChoice ("highSlope", "High Slope", slopes, 1.0f),
        { .id = "lmhDelay", .name = "LMH Delay", .maximum = 5.0f, .defaultValue = 2.0f, .unit = Unit::milliseconds },
        { .id = "lmhMix", .name = "LMH Mix", .defaultValue = 1.0f, .unit = Unit::percent },
        { .id = "midDrivesMaster", .name = "Mid Drives Master", .maximum = 1.0f, .unit = Unit::toggle },
        PHAZITE_MULTIBAND_BAND ("low", "Low", 10.0f),
        PHAZITE_MULTIBAND_BAND ("mid", "Mid", 10.0f),
        PHAZITE_MULTIBAND_BAND ("high", "High", 10.0f),
        PHAZITE_MULTIBAND_BAND ("master", "Master", 2.0f),
    };

#undef PHAZITE_MULTIBAND_BAND
#undef PHAZITE_MULTIBAND_POINT

    static_assert (std::size (parameters) == (size_t) Multiband::getBandParameter (Multiband::numBands, Multiband::mode));
    static_assert (Multiband::getPointParameter (Multiband::master, Multiband::maxPoints - 1, Multiband::pointType) + 1
                   == (int) std::size (parameters));

    constexpr double gainSeconds = 0.02;  // how long a change of a band's pre or post gain takes, about
    constexpr float log9 = 2.1972245773f; // the curves' axis: ln 9
    constexpr float quietest = -120.0f;   // decibels: no quieter gains than this
    constexpr float loudest = 48.0f;      // and no louder

    int roundToChoice (float value, int count) noexcept
    {
        return juce::jlimit (0, count - 1, (int) std::lround (value));
    }

    // A line rising (or falling) from 0 to 1 over a fraction s of its way, bent up by a tension above 0 or down below it:
    // (e^(-a s) - 1) / (e^(-a) - 1), the bend a = 8 tension (as the synths' envelopes bend, devices/dsp/Envelopes.h).
    float bendUp (float s, float bend) noexcept
    {
        if (std::abs (bend) < 1.0e-4f)
            return s;

        return (float) (std::expm1 (-(double) bend * s) / std::expm1 (-(double) bend));
    }

    float getLine (Multiband::PointType type, float tension, float s, float from, float to) noexcept
    {
        // Bending "up" means faster first for a rising line, slower first for a falling one.
        const auto bend = 8.0f * tension * (to >= from ? 1.0f : -1.0f);

        switch (type)
        {
            case Multiband::PointType::doubleCurve:
            {
                // An S: the first half slow then fast, the second its mirror, the tension how steep its middle is.
                const auto middle = 0.5f * (from + to);

                if (s < 0.5f)
                    return from + (middle - from) * bendUp (2.0f * s, -bend);

                return middle + (to - middle) * (1.0f - bendUp (2.0f - 2.0f * s, -bend));
            }
            case Multiband::PointType::hold:
                return s < 1.0f ? from : to;
            case Multiband::PointType::stairs:
            {
                const auto steps = Multiband::getStairs (tension);
                const auto step = std::min ((int) std::floor (s * (float) steps), steps - 1);
                return from + (to - from) * (float) step / (float) (steps - 1);
            }
            case Multiband::PointType::smoothStairs:
            {
                const auto steps = Multiband::getStairs (tension);
                const auto along = s * (float) steps;
                const auto step = std::floor (along);
                const auto rounded = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (along - step));
                return from + (to - from) * std::min (1.0f, (step + rounded) / (float) steps);
            }
            case Multiband::PointType::singleCurve:
            default:
                return from + (to - from) * bendUp (s, bend);
        }
    }

    // The saturation: up to its threshold the sound passes as it is, then it rounds off into the ceiling (type A, softly, as
    // tanh does) or bends more firmly (type B, a cubic that reaches the ceiling flat). The further the knob turns either way,
    // the lower the threshold, down to 24 dB under the ceiling.
    float saturate (float x, float amount, float ceiling) noexcept
    {
        const auto threshold = ceiling * dsp::decibelsToGain (-24.0f * std::abs (amount));
        const auto size = std::abs (x);

        if (size <= threshold || ceiling <= threshold)
            return x;

        const auto room = ceiling - threshold;
        const auto over = (size - threshold) / room;
        const auto shaped = amount < 0.0f ? std::tanh (over) : (over < 1.5f ? over - (4.0f / 27.0f) * over * over * over : 1.0f);
        return std::copysign (threshold + room * shaped, x);
    }

    // A band's stereo separation, from mono (-1) through as it is (0) to twice as wide (1), through its middle and sides.
    void separate (float& left, float& right, float amount) noexcept
    {
        if (amount == 0.0f)
            return;

        const auto middle = 0.5f * (left + right), side = 0.5f * (left - right) * (1.0f + amount);
        left = middle + side;
        right = middle - side;
    }

    float getCoefficient (double seconds, double sampleRate) noexcept
    {
        return seconds <= 0.0 ? 1.0f : (float) (1.0 - std::exp (-1.0 / (seconds * sampleRate)));
    }
} // namespace

const DeviceInfo Multiband::info { .typeID = typeID, .name = "Multiband", .kind = DeviceKind::effect, .parameters = parameters };

float Multiband::toAxis (float amplitude) noexcept
{
    return std::log1p (2.0f * std::max (0.0f, amplitude)) / log9;
}

float Multiband::fromAxis (float place) noexcept
{
    return 0.5f * std::expm1 (place * log9);
}

float Multiband::axisForDecibels (float decibels) noexcept
{
    return toAxis (dsp::decibelsToGain (decibels));
}

int Multiband::getStairs (float tension) noexcept
{
    return 2 + (int) std::lround ((juce::jlimit (-1.0f, 1.0f, tension) + 1.0f) * 7.0f);
}

float Multiband::getCurveAt (std::span<const float> values, int band, float place)
{
    const auto value = [values, band] (int point, PointField field) { return values[(size_t) getPointParameter (band, point, field)]; };
    const auto count = juce::jlimit (2, maxPoints, (int) std::lround (values[(size_t) getBandParameter (band, points)]));

    // Each point no further left than the one before it.
    auto lastX = 0.0f, lastY = value (0, pointY);

    if (place <= value (0, pointX))
        return lastY;

    lastX = std::max (0.0f, value (0, pointX));

    for (int i = 1; i < count; ++i)
    {
        const auto x = std::max (lastX, value (i, pointX)), y = value (i, pointY);

        if (place <= x)
        {
            const auto s = x > lastX ? (place - lastX) / (x - lastX) : 1.0f;
            const auto type = (PointType) roundToChoice (value (i, pointType), (int) std::size (pointTypes));
            return getLine (type, value (i, pointTension), s, lastY, y);
        }

        lastX = x;
        lastY = y;
    }

    return lastY;
}

void Multiband::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    lookahead = std::max (1, (int) std::lround (mostLookaheadSeconds * sampleRate));
    preSmoothing = getCoefficient (gainSeconds, sampleRate);

    for (auto* delays : { &dryDelays, &masterDelays })
        for (auto& delay : *delays)
            delay.prepare (lookahead);

    for (auto& band : bandDelays)
        for (auto& delay : band)
            delay.prepare (lookahead);

    for (auto& compressor : compressors)
    {
        compressor.heard.prepare (lookahead);
        compressor.hasTable = false; // made again from the parameters at the first block
    }

    reset();
}

void Multiband::reset()
{
    for (auto& filter : lowCuts)
        filter.reset();

    for (auto* crossovers : { &lows, &highs })
        for (auto& crossover : *crossovers)
            for (auto& channel : crossover)
                for (auto& filter : channel)
                    filter.reset();

    for (auto& phase : lowPhases)
        phase.reset();

    for (auto& crossover : dryPhases)
        for (auto& phase : crossover)
            phase.reset();

    for (auto* delays : { &dryDelays, &masterDelays })
        for (auto& delay : *delays)
            delay.reset();

    for (auto& band : bandDelays)
        for (auto& delay : band)
            delay.reset();

    for (auto& compressor : compressors)
    {
        compressor.heard.reset();
        compressor.meanSquare = compressor.held = 0.0f;
        compressor.heldFor = 0;
        compressor.attackStages = {};
        compressor.releaseStages = {};
        compressor.ramp = compressor.releaseFrom = compressor.gain = 0.0f;
        compressor.releasedFor = -1;
        compressor.linearGain = 1.0f;
        compressor.shownGain = 0.0f;
    }

    isFirstBlock = true;
}

void Multiband::updateCurve (Compressor& compressor, std::span<const float> values, int band)
{
    // Made again only when the curve's parameters change.
    const auto first = (size_t) getBandParameter (band, points);
    const auto count = compressor.shownCurve.size();

    if (compressor.hasTable && std::equal (compressor.shownCurve.begin(), compressor.shownCurve.end(), values.begin() + (long) first))
        return;

    std::copy (values.begin() + (long) first, values.begin() + (long) (first + count), compressor.shownCurve.begin());
    compressor.hasTable = true;

    // The gain for each place across the axis: how far out the curve puts it, against how far in it is.
    for (int i = 0; i <= tableSize; ++i)
    {
        const auto place = std::max (1.0e-4f, (float) i / (float) tableSize);
        const auto in = fromAxis (place), out = fromAxis (getCurveAt (values, band, place));
        compressor.gains[(size_t) i] = juce::jlimit (quietest, loudest, dsp::gainToDecibels (out) - dsp::gainToDecibels (in));
    }

    compressor.lastGain = fromAxis (getCurveAt (values, band, 1.0f));
}

float Multiband::compress (Compressor& c, std::span<const float> values, int band, float level, bool compresses) noexcept
{
    const auto value = [values, band] (BandField field) { return values[(size_t) getBandParameter (band, field)]; };

    // The gain the curve wants for what is heard now, in decibels; past the axis's end, as loud as the curve's end.
    auto wanted = 0.0f;

    if (compresses)
    {
        const auto place = toAxis (level);

        if (place >= 1.0f)
        {
            wanted = juce::jlimit (quietest, loudest, dsp::gainToDecibels (c.lastGain) - dsp::gainToDecibels (level));
        }
        else
        {
            const auto along = place * (float) tableSize;
            const auto index = std::min ((int) along, tableSize - 1);
            wanted = c.gains[(size_t) index] + (along - (float) index) * (c.gains[(size_t) index + 1] - c.gains[(size_t) index]);
        }
    }

    // The attack: through as many smoothing stages as the curve box says, the attack's time shared among them, so that a
    // higher number starts more roundly. The first release: speeding up from where it began, reaching the gain wanted as its
    // time is up, its curve box how much it speeds up. The second release: on the way up, through the same stages, slowing.
    const auto stages = roundToChoice (value (curve), 8) + 1;
    const auto attackShare = getCoefficient (value (attack) * 0.001 / stages, sampleRate);

    if (wanted < c.ramp)
    {
        c.releasedFor = -1;
        auto input = wanted;

        for (int k = 0; k < stages; ++k)
        {
            c.attackStages[(size_t) k] += (input - c.attackStages[(size_t) k]) * attackShare;
            input = c.attackStages[(size_t) k];
        }

        c.ramp = std::max (input, wanted);
    }
    else
    {
        if (c.releasedFor < 0)
        {
            c.releasedFor = 0;
            c.releaseFrom = c.ramp;
        }

        const auto releaseSamples = value (release) * 0.001f * (float) sampleRate;
        const auto power = 1.0f + 0.5f * (float) roundToChoice (value (releaseCurve), 8);

        if (releaseSamples < 1.0f || c.releasedFor >= (int) releaseSamples)
            c.ramp = wanted;
        else
            c.ramp = c.releaseFrom + (wanted - c.releaseFrom) * std::pow ((float) c.releasedFor / releaseSamples, power);

        ++c.releasedFor;

        for (int k = 0; k < stages; ++k)
            c.attackStages[(size_t) k] = c.ramp;
    }

    if (c.ramp <= c.gain)
    {
        c.gain = c.ramp;

        for (int k = 0; k < stages; ++k)
            c.releaseStages[(size_t) k] = c.ramp;
    }
    else
    {
        const auto releaseShare = getCoefficient (value (release2) * 0.001 / stages, sampleRate);
        auto input = c.ramp;

        for (int k = 0; k < stages; ++k)
        {
            c.releaseStages[(size_t) k] += (input - c.releaseStages[(size_t) k]) * releaseShare;
            input = c.releaseStages[(size_t) k];
        }

        c.gain = input;
    }

    if (! juce::exactlyEqual (c.gain, c.shownGain))
    {
        c.shownGain = c.gain;
        c.linearGain = dsp::decibelsToGain (c.gain);
    }

    return c.linearGain;
}

void Multiband::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);

    if (numChannels <= 0 || block.numSamples <= 0)
        return;

    const auto global = [&values] (Global g) { return values[(size_t) g]; };
    const auto bandValue = [&values] (int band, BandField field) { return values[(size_t) getBandParameter (band, field)]; };
    const auto bandMode = [&] (int band) { return (Mode) roundToChoice (bandValue (band, mode), (int) std::size (modes)); };

    for (int band = 0; band < numBands; ++band)
        updateCurve (compressors[(size_t) band], values, band);

    // The filters: the low cut, and each crossover at 12 dB per octave (one section each way, the high one upside down, so
    // that they add up to a first-order all-pass) or 24 (two Butterworth sections each way, adding up to a second-order one).
    const auto cut = dsp::makeFilter (dsp::FilterShape::highPass, global (Global::lowCut), sampleRate, 0.70710678);
    const auto lowSplit = std::min ((double) global (Global::lowFrequency), 0.45 * sampleRate);
    const auto highSplit = juce::jlimit (lowSplit, 0.45 * sampleRate, (double) global (Global::highFrequency));
    const std::array frequencies { lowSplit, highSplit };
    const std::array steep { roundToChoice (global (lowSlope), 2) == 1, roundToChoice (global (highSlope), 2) == 1 };
    std::array<std::array<dsp::FilterCoefficients, 2>, 2> lowPass {}, highPass {};
    std::array<dsp::FilterCoefficients, 2> allPass {};
    std::array<float, 2> firstOrder {};

    for (size_t i = 0; i < 2; ++i)
    {
        const auto q = steep[i] ? 0.70710678 : 0.5;
        lowPass[i][0] = lowPass[i][1] = dsp::makeFilter (dsp::FilterShape::lowPass, frequencies[i], sampleRate, q);
        highPass[i][0] = highPass[i][1] = dsp::makeFilter (dsp::FilterShape::highPass, frequencies[i], sampleRate, q);
        allPass[i] = dsp::makeFilter (dsp::FilterShape::allPass, frequencies[i], sampleRate, 0.70710678);
        const auto tangent = std::tan (juce::MathConstants<double>::pi * frequencies[i] / sampleRate);
        firstOrder[i] = (float) ((tangent - 1.0) / (tangent + 1.0));
    }

    // What the bands and the master look ahead by: the bands by LMH Delay, the master by its attack, each up to the latency's
    // half, by delaying what their compressors hear less than the sound.
    const auto bandAhead = juce::jlimit (0, lookahead, (int) std::lround (global (lmhDelay) * 0.001 * sampleRate));
    const auto masterAhead = juce::jlimit (0, lookahead, (int) std::lround (bandValue (master, attack) * 0.001 * sampleRate));
    const auto midDrives = global (midDrivesMaster) > 0.5f;
    const auto lmh = global (lmhMix);

    std::array<Mode, numBands> bandModes {};
    std::array<float, numBands> targetPre {}, targetPost {}, saturations {}, ceilings {}, widths {};
    std::array<int, numBands> holdSamples {};
    std::array<float, numBands> rmsShare {};
    std::array<bool, numBands> averages {}, compresses {};
    auto soloed = false;

    for (int band = 0; band < numBands; ++band)
    {
        const auto b = (size_t) band;
        bandModes[b] = bandMode (band);
        targetPre[b] = dsp::decibelsToGain (bandValue (band, pre));
        targetPost[b] = dsp::decibelsToGain (bandValue (band, post));
        saturations[b] = bandValue (band, saturation);
        ceilings[b] = dsp::decibelsToGain (bandValue (band, ceiling));
        widths[b] = bandValue (band, stereo);
        holdSamples[b] = (int) std::lround (bandValue (band, sustain) * 0.001 * sampleRate);
        rmsShare[b] = getCoefficient (std::max (0.001, bandValue (band, sustain) * 0.001), sampleRate);
        averages[b] = roundToChoice (bandValue (band, detection), 2) == 1;
        compresses[b] = bandModes[b] == Mode::on && ! (midDrives && (band == low || band == high));
        soloed = soloed || (band != master && bandValue (band, solo) > 0.5f && bandModes[b] != Mode::off);
    }

    if (isFirstBlock)
    {
        preGains = targetPre;
        postGains = targetPost;
        isFirstBlock = false;
    }

    // What a band's detector hears from a pair of samples: its peak, held for its sustain, or its average over it.
    const auto hear = [&] (int band, float left, float right)
    {
        const auto b = (size_t) band;
        auto& c = compressors[b];

        if (averages[b])
        {
            c.meanSquare += (std::max (left * left, right * right) - c.meanSquare) * rmsShare[b];
            return std::sqrt (2.0f * c.meanSquare);
        }

        const auto peak = std::max (std::abs (left), std::abs (right));

        if (peak >= c.held || c.heldFor <= 0)
        {
            c.held = peak;
            c.heldFor = holdSamples[b];
        }
        else
        {
            --c.heldFor;
        }

        return c.held;
    };

    std::array<float, numBands> loudestIn {}, loudestOut {}, loudestHeard {}, most {};
    std::fill (most.begin(), most.end(), -1000.0f);
    auto loudestMain = 0.0f;
    auto* left = block.channels[0];
    auto* right = numChannels > 1 ? block.channels[1] : nullptr;

    for (int i = 0; i < block.numSamples; ++i)
    {
        std::array<float, 2> input { left[i], right != nullptr ? right[i] : left[i] };
        loudestMain = std::max ({ loudestMain, std::abs (input[0]), std::abs (input[1]) });

        // The low cut, then the split into three bands; and the dry sound, in the phase the bands add up to and delayed as they
        // are.
        std::array<std::array<float, 2>, 3> bands {};
        std::array<float, 2> dry {};

        for (size_t channel = 0; channel < 2; ++channel)
        {
            const auto x = lowCuts[channel].process (input[channel], cut);
            auto phased = x;

            for (size_t crossover = 0; crossover < 2; ++crossover)
                phased = dryPhases[crossover][channel].process (phased, steep[crossover], allPass[crossover], firstOrder[crossover]);

            dryDelays[channel].push (phased);
            dry[channel] = dryDelays[channel].read (lookahead);

            const auto split = [&] (size_t crossover, float sound, float& below, float& above)
            {
                auto& lowSections = lows[crossover][channel];
                auto& highSections = highs[crossover][channel];
                below = lowSections[0].process (sound, lowPass[crossover][0]);
                above = highSections[0].process (sound, highPass[crossover][0]);

                if (steep[crossover])
                {
                    below = lowSections[1].process (below, lowPass[crossover][1]);
                    above = highSections[1].process (above, highPass[crossover][1]);
                }
                else
                {
                    above = -above;
                }
            };

            float lowBand = 0.0f, upper = 0.0f, midBand = 0.0f, highBand = 0.0f;
            split (0, x, lowBand, upper);
            split (1, upper, midBand, highBand);
            lowBand = lowPhases[channel].process (lowBand, steep[1], allPass[1], firstOrder[1]);
            bands[0][channel] = lowBand;
            bands[1][channel] = midBand;
            bands[2][channel] = highBand;
        }

        // A low or high band that is off goes into the mid band, which then covers its room.
        for (const auto band : { low, high })
        {
            if (bandModes[(size_t) band] == Mode::off)
            {
                for (size_t channel = 0; channel < 2; ++channel)
                {
                    bands[1][channel] += bands[(size_t) band][channel];
                    bands[(size_t) band][channel] = 0.0f;
                }
            }
        }

        // Each band: its stereo separation, its pre gain, what its compressor hears (ahead of the sound), the gain, its post
        // gain and its saturation; the mid band's sound, as its compressor gets it, for a master the mid band drives.
        std::array<float, 2> sum {}, midHeard {};

        for (int band = low; band <= high; ++band)
        {
            const auto b = (size_t) band;
            auto [l, r] = bands[b];
            preGains[b] += (targetPre[b] - preGains[b]) * preSmoothing;
            postGains[b] += (targetPost[b] - postGains[b]) * preSmoothing;

            if (bandModes[b] != Mode::off)
            {
                separate (l, r, widths[b]);
                l *= preGains[b];
                r *= preGains[b];
            }

            auto& c = compressors[b];
            const auto heard = hear (band, l, r);
            c.heard.push (heard);
            const auto ahead = c.heard.read (lookahead - bandAhead);

            bandDelays[b][0].push (l);
            bandDelays[b][1].push (r);
            l = bandDelays[b][0].read (lookahead);
            r = bandDelays[b][1].read (lookahead);

            if (band == mid)
                midHeard = { l, r };

            if (bandModes[b] == Mode::off)
            {
                // As it came in (already delayed as the others are).
            }
            else
            {
                const auto gain = compress (c, values, band, ahead, compresses[b]);
                l = saturate (l * gain * postGains[b], saturations[b], ceilings[b]);
                r = saturate (r * gain * postGains[b], saturations[b], ceilings[b]);
                most[b] = std::max (most[b], -c.gain);
            }

            loudestIn[b] = std::max (loudestIn[b], std::max (std::abs (bandDelays[b][0].read (0)), std::abs (bandDelays[b][1].read (0))));
            loudestHeard[b] = std::max (loudestHeard[b], heard);

            if (bandModes[b] == Mode::muted || (soloed && bandValue (band, solo) <= 0.5f))
                l = r = 0.0f;

            loudestOut[b] = std::max ({ loudestOut[b], std::abs (l), std::abs (r) });
            sum[0] += l;
            sum[1] += r;
        }

        // The master band, on the bands mixed with the dry sound.
        const auto m = (size_t) master;
        auto l = dry[0] + lmh * (sum[0] - dry[0]);
        auto r = dry[1] + lmh * (sum[1] - dry[1]);
        preGains[m] += (targetPre[m] - preGains[m]) * preSmoothing;
        postGains[m] += (targetPost[m] - postGains[m]) * preSmoothing;

        if (bandModes[m] != Mode::off)
        {
            separate (l, r, widths[m]);
            l *= preGains[m];
            r *= preGains[m];
        }

        auto& c = compressors[m];
        const auto heard = midDrives ? hear (master, midHeard[0] * preGains[m], midHeard[1] * preGains[m]) : hear (master, l, r);
        c.heard.push (heard);
        const auto ahead = c.heard.read (lookahead - masterAhead);
        loudestIn[m] = std::max ({ loudestIn[m], std::abs (l), std::abs (r) });
        loudestHeard[m] = std::max (loudestHeard[m], heard);
        masterDelays[0].push (l);
        masterDelays[1].push (r);
        l = masterDelays[0].read (lookahead);
        r = masterDelays[1].read (lookahead);

        if (bandModes[m] == Mode::muted)
        {
            l = r = 0.0f;
        }
        else if (bandModes[m] != Mode::off)
        {
            const auto gain = compress (c, values, master, ahead, compresses[m]);
            l = saturate (l * gain * postGains[m], saturations[m], ceilings[m]);
            r = saturate (r * gain * postGains[m], saturations[m], ceilings[m]);
            most[m] = std::max (most[m], -c.gain);
        }

        loudestOut[m] = std::max ({ loudestOut[m], std::abs (l), std::abs (r) });
        left[i] = l;

        if (right != nullptr)
            right[i] = r;
    }

    for (size_t band = 0; band < (size_t) numBands; ++band)
    {
        inputMeters[band].raise (dsp::gainToDecibels (loudestIn[band]));
        outputMeters[band].raise (dsp::gainToDecibels (loudestOut[band]));
        heardMeters[band].raise (dsp::gainToDecibels (loudestHeard[band]));

        if (most[band] > -1000.0f)
            reductionMeters[band].raise (most[band]);
    }

    mainInputMeter.raise (dsp::gainToDecibels (loudestMain));
    gainReduction.push (std::max (0.0f, most[(size_t) master]));
}
} // namespace phazite::devices
