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

    constexpr ParameterInfo makeToggle (std::string_view id, std::string_view name)
    {
        return { .id = id, .name = name, .maximum = 1.0f, .unit = Unit::toggle };
    }

    // A point of a band's curve, from its band's prefix: at first the curve is a limiter at 0 dB, as Maximus's default is: up
    // the diagonal to 0 dB (the middle), then level to the end.
#define PHAZITE_MULTIBAND_POINT(band, Band, n, x, y)                                                                                       \
    { .id = band "Point" #n "X", .name = Band " Point " #n " X", .defaultValue = x, .unit = Unit::percent },                               \
        { .id = band "Point" #n "Y", .name = Band " Point " #n " Y", .defaultValue = y, .unit = Unit::percent },                           \
        makeBipolar (band "Point" #n "Tension", Band " Point " #n " Tension"),                                                             \
        makeChoice (band "Point" #n "Type", Band " Point " #n " Type", curves::pointTypeNames, 0.0f)

    // A band's parameters, in the order of Multiband::BandField. The master looks ahead by its attack, so it starts quicker.
#define PHAZITE_MULTIBAND_BAND(band, Band, attackDefault)                                                                                  \
    makeChoice (band "Mode", Band " Mode", modes, 0.0f), makeBipolar (band "Stereo", Band " Stereo"),                                      \
        makeDecibels (band "Pre", Band " Pre", -24.0f, 24.0f), makeDecibels (band "Post", Band " Post", -24.0f, 24.0f),                    \
        makeTime (band "Attack", Band " Attack", 500.0f, attackDefault, 20.0f),                                                            \
        makeTime (band "Release", Band " Release", 2000.0f, 0.0f, 200.0f),                                                                 \
        makeTime (band "Release2", Band " Release 2", 2000.0f, 100.0f, 200.0f),                                                            \
        makeTime (band "Sustain", Band " Sustain", 1000.0f, 20.0f, 100.0f), makeChoice (band "Curve", Band " Curve", curveShapes, 1.0f),   \
        makeChoice (band "ReleaseCurve", Band " Release Curve", curveShapes, 2.0f),                                                        \
        makeChoice (band "Detection", Band " Detection", detections, 0.0f), makeBipolar (band "Saturation", Band " Saturation"),           \
        makeDecibels (band "Ceiling", Band " Ceiling", -24.0f, 6.0f), makeToggle (band "Key", Band " Key"),                                \
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
        makeToggle ("midDrivesMaster", "Mid Drives Master"),
        makeToggle ("lowSolo", "Low Solo"),
        makeToggle ("midSolo", "Mid Solo"),
        makeToggle ("highSolo", "High Solo"),
        { .id = "keyLowCut",
          .name = "Key Low Cut",
          .minimum = 20.0f,
          .maximum = 500.0f,
          .defaultValue = 20.0f,
          .unit = Unit::hertz,
          .midpoint = 100.0f },
        { .id = "keyHighCut",
          .name = "Key High Cut",
          .minimum = 100.0f,
          .maximum = 20000.0f,
          .defaultValue = 20000.0f,
          .unit = Unit::hertz,
          .midpoint = 1500.0f },
        PHAZITE_MULTIBAND_BAND ("low", "Low", 10.0f),
        PHAZITE_MULTIBAND_BAND ("mid", "Mid", 10.0f),
        PHAZITE_MULTIBAND_BAND ("high", "High", 10.0f),
        PHAZITE_MULTIBAND_BAND ("master", "Master", 2.0f),
    };

#undef PHAZITE_MULTIBAND_BAND
#undef PHAZITE_MULTIBAND_POINT

    static_assert (std::size (parameters) == (size_t) Multiband::getBandParameter (Multiband::numBands, Multiband::mode));
    static_assert (Multiband::getPointParameter (Multiband::master, curves::maxPoints - 1, curves::pointType) + 1
                   == (int) std::size (parameters));
    static_assert (Multiband::getSoloParameter (Multiband::high) == Multiband::highSolo);

    constexpr double gainSeconds = 0.02;      // how long a change of a band's pre or post gain takes, about
    constexpr float keyHighCutOff = 19999.0f; // the key's high cut at its top: off
    constexpr double keyFadeSeconds = 0.005;  // between the key and what it puts out, as Listen turns on or off

    int roundToChoice (float value, int count) noexcept
    {
        return juce::jlimit (0, count - 1, (int) std::lround (value));
    }

    // A band's stereo separation, from mono (-1) through as it is (0) to twice as wide (1), through its middle and sides.
    void separate (float& left, float& right, float amount) noexcept
    {
        if (juce::exactlyEqual (amount, 0.0f))
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

const DeviceInfo Multiband::info { .typeID = typeID,
                                   .name = "Multiband",
                                   .kind = DeviceKind::effect,
                                   .parameters = parameters,
                                   .hasSidechain = true };

// The saturation: up to its threshold the sound passes as it is, then it rounds off into the ceiling (type A, softly, as
// tanh does) or bends more firmly (type B, a cubic that reaches the ceiling flat). The further the knob turns either way,
// the lower the threshold, down to 24 dB under the ceiling (process() sets it).
float Multiband::SaturationCurve::value (float x) const noexcept
{
    const auto size = std::abs (x);

    if (size <= threshold || room <= 0.0f)
        return x;

    const auto over = (size - threshold) / room;
    const auto shaped = soft ? std::tanh (over) : (over < 1.5f ? over - (4.0f / 27.0f) * over * over * over : 1.0f);
    return std::copysign (threshold + room * shaped, x);
}

// Its integral from 0: x^2 / 2 up to the threshold; past it, the threshold's level for the rest of the way and the room
// squared times the bend's integral, ln cosh for tanh, o^2 / 2 - o^4 / 27 for the cubic (then straight, from 15/16 on).
double Multiband::SaturationCurve::integral (double x) const noexcept
{
    const auto size = std::abs (x), below = (double) threshold, r = (double) room;

    if (size <= below || r <= 0.0)
        return 0.5 * x * x;

    const auto over = (size - below) / r;
    const auto bent = soft ? over + std::log1p (std::exp (-2.0 * over)) - 0.69314718055994531
                           : (over < 1.5 ? 0.5 * over * over - over * over * over * over / 27.0 : 0.9375 + (over - 1.5));
    return 0.5 * below * below + below * (size - below) + r * r * bent;
}

bool Multiband::SaturationCurve::isSame (const SaturationCurve& other) const noexcept
{
    return juce::exactlyEqual (threshold, other.threshold) && juce::exactlyEqual (room, other.room) && soft == other.soft;
}

void Multiband::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    lookahead = std::max (1, (int) std::lround (mostLookaheadSeconds * sampleRate));
    saturationLatency = (int) std::lround (dsp::AntialiasedShaper::getLatencySamples());
    preSmoothing = getCoefficient (gainSeconds, sampleRate);

    for (auto* lines : { &bandsInStep, &masterInStep })
        for (auto& line : *lines)
            line.prepare (std::max (1, saturationLatency));

    for (auto* delays : { &dryDelays, &masterDelays })
        for (auto& delay : *delays)
            delay.prepare (lookahead);

    for (auto& band : bandDelays)
        for (auto& delay : band)
            delay.prepare (lookahead);

    for (auto& compressor : compressors)
    {
        compressor.heard.prepare (lookahead);
        compressor.gains.reset(); // made again from the parameters at the first block
    }

    keyStep = (float) std::min (1.0, 1.0 / (keyFadeSeconds * sampleRate));

    for (auto& line : keyDelays)
        line.prepare (getLatencySamples());

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

    for (auto& band : saturators)
        for (auto& saturator : band)
            saturator.reset();

    for (auto& inStep : dryInStep)
        inStep.reset();

    for (auto* lines : { &bandsInStep, &masterInStep })
        for (auto& line : *lines)
            line.reset();

    lastCurves = {};
    bandsSaturated = masterSaturated = false;

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

    resetKey();
    keyShare = 0.0f;
    keyRunning = false;
    isFirstBlock = true;
}

void Multiband::resetKey() noexcept
{
    for (auto& filter : keyLowCuts)
        filter.reset();

    for (auto& channel : keyHighCuts)
        for (auto& filter : channel)
            filter.reset();

    for (auto& line : keyDelays)
        line.reset();
}

float Multiband::compress (Compressor& c, std::span<const float> values, int band, float level, bool compresses) noexcept
{
    const auto value = [values, band] (BandField field) { return values[(size_t) getBandParameter (band, field)]; };

    // The gain the curve wants for what is heard now, in decibels; past the axis's end, as loud as the curve's end.
    auto wanted = 0.0f;

    if (compresses)
        wanted = c.gains.getDecibels (level);

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
        compressors[(size_t) band].gains.update (getCurve (values, band));

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
    std::array<bool, numBands> averages {}, compresses {}, hearsKey {}, solos {};
    auto soloed = false, anyHearsKey = false;

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
        hearsKey[b] = bandValue (band, key) > 0.5f;
        anyHearsKey = anyHearsKey || hearsKey[b];

        if (band != master)
        {
            solos[b] = values[(size_t) getSoloParameter (band)] > 0.5f;
            soloed = soloed || (solos[b] && bandModes[b] != Mode::off);
        }
    }

    // The key: what the sidechain feeds, or with none what comes in, through its low cut and (unless it is at its top) its
    // high cut, as the compressor's key; worked out while a band hears it or Listen plays it.
    const float* const* keyIn = nullptr; // the sidechain's channels; none for what comes in
    auto numKeyChannels = 0;

    if (block.sidechain != nullptr && block.numSidechainChannels > 0)
    {
        keyIn = block.sidechain;
        numKeyChannels = std::min (block.numSidechainChannels, 2);
    }

    const auto keyWanted = isPlayingKey() ? 1.0f : 0.0f;
    const auto keyNeeded = anyHearsKey || keyWanted > 0.0f || keyShare > 0.0f;

    // Worked out again, it starts from silence, so that neither a band nor Listen hears it from where it stopped.
    if (keyNeeded && ! keyRunning)
        resetKey();

    keyRunning = keyNeeded;
    const auto keyLow = dsp::makeFilter (dsp::FilterShape::highPass, global (Global::keyLowCut), sampleRate, 0.70710678);
    const auto keyCutsHigh = global (Global::keyHighCut) < keyHighCutOff;
    const auto keyHighFrequency = std::min ((double) global (Global::keyHighCut), 0.45 * sampleRate);
    const std::array keyHigh { dsp::makeFilter (dsp::FilterShape::lowPass, keyHighFrequency, sampleRate, dsp::getButterworthQ (4, 0)),
                               dsp::makeFilter (dsp::FilterShape::lowPass, keyHighFrequency, sampleRate, dsp::getButterworthQ (4, 1)) };
    const auto latency = getLatencySamples();

    if (isFirstBlock)
    {
        preGains = targetPre;
        postGains = targetPost;
        isFirstBlock = false;
    }

    // Each band's saturation as a curve (a band that is off, a straight line), its averaging started again when it changes.
    std::array<SaturationCurve, numBands> saturationCurves {};
    const SaturationCurve straight;

    for (int band = 0; band < numBands; ++band)
    {
        const auto b = (size_t) band;
        auto& bandCurve = saturationCurves[b];

        if (bandModes[b] != Mode::off)
        {
            const auto threshold = ceilings[b] * dsp::decibelsToGain (-24.0f * std::abs (saturations[b]));
            bandCurve.threshold = threshold;
            bandCurve.room = std::max (0.0f, ceilings[b] - threshold);
            bandCurve.soft = saturations[b] < 0.0f;
        }

        if (! bandCurve.isSame (lastCurves[b]))
        {
            for (auto& saturator : saturators[b])
                saturator.restartAveraging();

            lastCurves[b] = bandCurve;
        }
    }

    // Whether the bands go through their saturations (while any of them saturates), and the master through its own; each
    // starting from silence as it begins to.
    auto bandsSaturate = false;

    for (int band = low; band <= high; ++band)
        bandsSaturate = bandsSaturate || saturationCurves[(size_t) band].room > 0.0f;

    const auto masterSaturates = saturationCurves[(size_t) master].room > 0.0f;

    if (bandsSaturate && ! bandsSaturated)
    {
        for (int band = low; band <= high; ++band)
            for (auto& saturator : saturators[(size_t) band])
                saturator.reset();

        for (auto& inStep : dryInStep)
            inStep.reset();
    }

    if (masterSaturates && ! masterSaturated)
        for (auto& saturator : saturators[(size_t) master])
            saturator.reset();

    bandsSaturated = bandsSaturate;
    masterSaturated = masterSaturates;

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

        // The key, through its cuts.
        std::array<float, 2> keyed {};

        if (keyNeeded)
        {
            for (size_t channel = 0; channel < 2; ++channel)
            {
                auto x = keyIn != nullptr ? keyIn[std::min ((int) channel, numKeyChannels - 1)][i] : input[channel];
                x = keyLowCuts[channel].process (x, keyLow);

                if (keyCutsHigh)
                    x = keyHighCuts[channel][1].process (keyHighCuts[channel][0].process (x, keyHigh[0]), keyHigh[1]);

                keyed[channel] = x;
                keyDelays[channel].push (x);
            }
        }

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

            if (bandsSaturate)
                dry[channel] = dryInStep[channel].process (dry[channel], straight);

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

        // Each band: its stereo separation, its pre gain, what its compressor hears (its band, or the key while its Key is on,
        // ahead of the sound), the gain, its post gain and its saturation; the mid band's sound, as its compressor gets it, for
        // a master the mid band drives.
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
            const auto heard = hearsKey[b] ? hear (band, keyed[0], keyed[1]) : hear (band, l, r);
            c.heard.push (heard);
            const auto ahead = c.heard.read (lookahead - bandAhead);

            bandDelays[b][0].push (l);
            bandDelays[b][1].push (r);
            l = bandDelays[b][0].read (lookahead);
            r = bandDelays[b][1].read (lookahead);

            if (band == mid)
                midHeard = { l, r };

            // Its gain, then its saturation while the bands saturate (a band that is off, or doesn't saturate, through a
            // straight line at twice the rate, in step with them); a band that is off as it came in (already delayed as the
            // others are).
            if (bandModes[b] != Mode::off)
            {
                const auto gain = compress (c, values, band, ahead, compresses[b]);
                l *= gain * postGains[b];
                r *= gain * postGains[b];
                most[b] = std::max (most[b], -c.gain);
            }

            if (bandsSaturate)
            {
                l = saturators[b][0].process (l, saturationCurves[b]);
                r = saturators[b][1].process (r, saturationCurves[b]);
            }

            loudestIn[b] = std::max (loudestIn[b], std::max (std::abs (bandDelays[b][0].read (0)), std::abs (bandDelays[b][1].read (0))));
            loudestHeard[b] = std::max (loudestHeard[b], heard);

            if (bandModes[b] == Mode::muted || (soloed && ! solos[b]))
                l = r = 0.0f;

            loudestOut[b] = std::max ({ loudestOut[b], std::abs (l), std::abs (r) });
            sum[0] += l;
            sum[1] += r;
        }

        // The master band, on the bands mixed with the dry sound.
        const auto m = (size_t) master;
        auto l = dry[0] + lmh * (sum[0] - dry[0]);
        auto r = dry[1] + lmh * (sum[1] - dry[1]);

        // While the bands don't saturate, as late as if they did.
        bandsInStep[0].push (l);
        bandsInStep[1].push (r);

        if (! bandsSaturate)
        {
            l = bandsInStep[0].read (saturationLatency);
            r = bandsInStep[1].read (saturationLatency);
        }
        preGains[m] += (targetPre[m] - preGains[m]) * preSmoothing;
        postGains[m] += (targetPost[m] - postGains[m]) * preSmoothing;

        if (bandModes[m] != Mode::off)
        {
            separate (l, r, widths[m]);
            l *= preGains[m];
            r *= preGains[m];
        }

        // What the master's compressor hears: the key while its Key is on (as late as the bands, which its sound comes through),
        // else the mid band while it drives the master, else what it plays.
        auto& c = compressors[m];
        const auto heard = hearsKey[m] ? hear (master, keyDelays[0].read (lookahead + saturationLatency),
                                               keyDelays[1].read (lookahead + saturationLatency))
                           : midDrives ? hear (master, midHeard[0] * preGains[m], midHeard[1] * preGains[m])
                                       : hear (master, l, r);
        c.heard.push (heard);
        const auto ahead = c.heard.read (lookahead - masterAhead);
        loudestIn[m] = std::max ({ loudestIn[m], std::abs (l), std::abs (r) });
        loudestHeard[m] = std::max (loudestHeard[m], heard);
        masterDelays[0].push (l);
        masterDelays[1].push (r);
        l = masterDelays[0].read (lookahead);
        r = masterDelays[1].read (lookahead);

        if (bandModes[m] != Mode::off && bandModes[m] != Mode::muted)
        {
            const auto gain = compress (c, values, master, ahead, compresses[m]);
            l *= gain * postGains[m];
            r *= gain * postGains[m];
            most[m] = std::max (most[m], -c.gain);
        }

        // Its saturation, or while it doesn't saturate, a delay as long.
        masterInStep[0].push (l);
        masterInStep[1].push (r);

        if (masterSaturates)
        {
            l = saturators[m][0].process (l, saturationCurves[m]);
            r = saturators[m][1].process (r, saturationCurves[m]);
        }
        else
        {
            l = masterInStep[0].read (saturationLatency);
            r = masterInStep[1].read (saturationLatency);
        }

        if (bandModes[m] == Mode::muted)
            l = r = 0.0f;

        loudestOut[m] = std::max ({ loudestOut[m], std::abs (l), std::abs (r) });

        // Listen: the key, as late as what it puts out, faded in and out.
        keyShare += std::clamp (keyWanted - keyShare, -keyStep, keyStep);

        if (keyShare > 0.0f)
        {
            l += keyShare * (keyDelays[0].read (latency) - l);
            r += keyShare * (keyDelays[1].read (latency) - r);
        }

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
