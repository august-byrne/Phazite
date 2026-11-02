// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Modulators.h"
#include "Modulation.h"

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
namespace
{
    // How a modulator's cycle is timed (Tracktion's ModifierCommon): free, from where it was; to the transport, where the
    // song is; to notes, from the last note started. Its rate is in hertz or a length of the bar, in this order.
    enum Sync
    {
        free,
        transport,
        note
    };

    constexpr int hertz = 0, fourBars = 1, sixtyFourthDotted = 21;

    // A rate's length as a part of a bar (hertz: one).
    double getBarFraction (int rateType) noexcept
    {
        constexpr double dot = 1.5, triplet = 2.0 / 3.0;
        constexpr double fractions[] = { 1.0,
                                         4.0,
                                         2.0,
                                         1.0,
                                         1.0 / 2.0 * triplet,
                                         1.0 / 2.0,
                                         1.0 / 2.0 * dot,
                                         1.0 / 4.0 * triplet,
                                         1.0 / 4.0,
                                         1.0 / 4.0 * dot,
                                         1.0 / 8.0 * triplet,
                                         1.0 / 8.0,
                                         1.0 / 8.0 * dot,
                                         1.0 / 16.0 * triplet,
                                         1.0 / 16.0,
                                         1.0 / 16.0 * dot,
                                         1.0 / 32.0 * triplet,
                                         1.0 / 32.0,
                                         1.0 / 32.0 * dot,
                                         1.0 / 64.0 * triplet,
                                         1.0 / 64.0,
                                         1.0 / 64.0 * dot };
        return juce::isPositiveAndBelow (rateType, (int) std::size (fractions)) ? fractions[rateType] : 1.0;
    }

    // clang-format off
    constexpr ModulatorParameterInfo lfoParameters[] = { { "wave", 0.0f }, { "syncType", 0.0f }, { "rate", 1.0f }, { "rateType", 3.0f },
                                                         { "depth", 1.0f }, { "biopolar", 0.0f }, { "phase", 0.0f }, { "offset", 0.0f } };
    constexpr ModulatorParameterInfo followerParameters[] = { { "gainDb", 0.0f }, { "attack", 100.0f }, { "hold", 0.0f },
                                                              { "release", 500.0f }, { "depth", 1.0f }, { "offset", 0.0f },
                                                              { "lowPassEnabled", 0.0f }, { "highPassEnabled", 0.0f },
                                                              { "lowPassFrequency", 2000.0f }, { "highPassFrequency", 500.0f } };
    constexpr ModulatorParameterInfo stepsParameters[] = { { "syncType", 0.0f }, { "numSteps", 16.0f }, { "rate", 1.0f },
                                                           { "rateType", 3.0f }, { "depth", 1.0f } };
    constexpr ModulatorParameterInfo randomParameters[] = { { "type", 0.0f }, { "shape", 0.0f }, { "syncType", 0.0f }, { "rate", 1.0f },
                                                            { "rateType", 3.0f }, { "depth", 1.0f }, { "stepDepth", 1.0f },
                                                            { "smooth", 0.0f }, { "biopolar", 0.0f } };
    constexpr ModulatorParameterInfo breakpointParameters[] = {
        { "numActivePoints", 4.0f }, { "syncType", 0.0f },        { "rate", 1.0f },          { "rateType", 3.0f },
        { "depth", 1.0f },           { "biopolar", 0.0f },        { "stageZeroValue", 0.0f }, { "stageOneValue", 1.0f },
        { "stageOneTime", 0.25f },   { "stageOneCurve", 0.0f },   { "stageTwoValue", 0.5f },  { "stageTwoTime", 0.5f },
        { "stageTwoCurve", 0.0f },   { "stageThreeValue", 0.5f }, { "stageThreeTime", 0.75f }, { "stageThreeCurve", 0.0f },
        { "stageFourValue", 0.0f },  { "stageFourTime", 1.0f },   { "stageFourCurve", 0.0f }
    };
    constexpr ModulatorParameterInfo noteFollowerParameters[] = { { "type", 0.0f }, { "mode", 0.0f }, { "relativeRoot", 60.0f },
                                                                  { "relativeSpread", 12.0f } };
    // clang-format on

    // Each kind's parameters by their places in the lists above.
    namespace lfo
    {
        enum
        {
            wave,
            syncType,
            rate,
            rateType,
            depth,
            bipolar,
            phase,
            offset
        };
    } // namespace lfo

    namespace follower
    {
        enum
        {
            gainDb,
            attack,
            hold,
            release,
            depth,
            offset,
            lowPassEnabled,
            highPassEnabled,
            lowPassFrequency,
            highPassFrequency
        };
    } // namespace follower

    namespace steps
    {
        enum
        {
            syncType,
            numSteps,
            rate,
            rateType,
            depth
        };
    } // namespace steps

    namespace randomSource
    {
        enum
        {
            type,
            shape,
            syncType,
            rate,
            rateType,
            depth,
            stepDepth,
            smooth,
            bipolar
        };
    } // namespace randomSource

    namespace breakpoints
    {
        enum
        {
            numActivePoints,
            syncType,
            rate,
            rateType,
            depth,
            bipolar,
            stageZeroValue
        };
    } // namespace breakpoints

    namespace noteFollower
    {
        enum
        {
            type,
            mode,
            relativeRoot,
            relativeSpread
        };
    } // namespace noteFollower

    // The parameters that time a cycle, by kind.
    struct Timing
    {
        int syncType, rate, rateType;
    };

    Timing getTiming (ModulatorKind kind) noexcept
    {
        switch (kind)
        {
            case ModulatorKind::lfo:
                return { lfo::syncType, lfo::rate, lfo::rateType };
            case ModulatorKind::steps:
                return { steps::syncType, steps::rate, steps::rateType };
            case ModulatorKind::random:
                return { randomSource::syncType, randomSource::rate, randomSource::rateType };
            case ModulatorKind::breakpoints:
                return { breakpoints::syncType, breakpoints::rate, breakpoints::rateType };
            case ModulatorKind::envelopeFollower:
            case ModulatorKind::noteFollower:
                break;
        }

        return { -1, -1, -1 };
    }

    float wrap (float phase) noexcept
    {
        while (phase >= 1.0f)
            phase -= 1.0f;

        while (phase < 0.0f)
            phase += 1.0f;

        return phase;
    }

    // A quadratic Bezier curve from one point to another, bent as a breakpoint envelope's stage is (curve -0.5 to 0.5),
    // where it is at x.
    float getBentValue (float x, float x1, float y1, float x2, float y2, float curve) noexcept
    {
        const auto c = juce::jlimit (-1.0f, 1.0f, curve * 2.0f);
        const auto run = x2 - x1;
        const auto xb = x1 + run / 2 + run / 2 * c;
        const auto yb = y2 > y1 ? y1 + (y2 - y1) / 2 - (y2 - y1) / 2 * c : y2 + (y1 - y2) / 2 + (y1 - y2) / 2 * c;

        if (juce::exactlyEqual (x1, x2) || juce::exactlyEqual (y1, y2))
            return y1;

        const auto a = x1 - 2 * xb + x2;
        const auto b = -2 * x1 + 2 * xb;
        const auto k = x1 - x;
        float t;

        if (juce::exactlyEqual (a, 0.0f))
        {
            t = -k / b;
        }
        else
        {
            const auto root = std::sqrt (b * b - 4 * a * k);
            t = (-b + root) / (2 * a);

            if (t < 0.0f || t > 1.0f)
                t = (-b - root) / (2 * a);
        }

        return (1 - t) * (1 - t) * y1 + 2 * t * (1 - t) * yb + t * t * y2;
    }
} // namespace

std::span<const ModulatorParameterInfo> getModulatorParameters (ModulatorKind kind)
{
    switch (kind)
    {
        case ModulatorKind::lfo:
            return lfoParameters;
        case ModulatorKind::envelopeFollower:
            return followerParameters;
        case ModulatorKind::steps:
            return stepsParameters;
        case ModulatorKind::random:
            return randomParameters;
        case ModulatorKind::breakpoints:
            return breakpointParameters;
        case ModulatorKind::noteFollower:
            return noteFollowerParameters;
    }

    return {};
}

int findModulatorParameter (ModulatorKind kind, std::string_view id)
{
    const auto parameters = getModulatorParameters (kind);

    for (size_t i = 0; i < parameters.size(); ++i)
        if (parameters[i].id == id)
            return (int) i;

    return -1;
}

Modulator::Modulator (ModulatorKind k)
    : kind (k)
{
}

void Modulator::prepare (double rate, std::uint32_t randomSeed)
{
    sampleRate = rate;
    seed = randomSeed;
    lowPassFrequency = highPassFrequency = 0.0f;
    reset();
}

void Modulator::reset()
{
    random.setSeed ((juce::int64) seed);
    ramp = 0.0;
    phase = 0.0f;
    previousRandom = currentRandom = 0.0f;
    noteStarted = false;
    envelope = 0.0f;
    holdLeft = 0;
    lowPass.reset();
    highPass.reset();
    lastNote = -1;
    lastVelocity = 0;
    noteValue = -1.0f; // as Tracktion's note follower starts: the lowest note's value
}

void Modulator::noteOn (int pitch, int velocity) noexcept
{
    noteStarted = true;
    lastNote = pitch;
    lastVelocity = velocity;
}

void Modulator::listen (const float* const* channels, int numChannels, int numSamples, std::span<const float> parameters) noexcept
{
    if (kind != ModulatorKind::envelopeFollower)
        return;

    // As Tracktion's envelope follower: the loudest channel's level, by its gain, through its filters, into an envelope
    // that rises at its attack, holds, and falls at its release (digital time constants: all but 14%, exp (-2), of the way
    // in that time).
    const auto getDelta = [this] (float milliseconds)
    { return std::exp (-2.0f / (std::max (milliseconds, 0.001f) * (float) sampleRate * 0.001f)); };
    const auto attack = getDelta (parameters[follower::attack]);
    const auto release = getDelta (parameters[follower::release]);
    const auto holdSamples = (int) (parameters[follower::hold] * 0.001 * sampleRate);
    const auto gain = juce::Decibels::decibelsToGain (parameters[follower::gainDb]);
    const auto lowPassOn = parameters[follower::lowPassEnabled] >= 0.5f, highPassOn = parameters[follower::highPassEnabled] >= 0.5f;

    if (! juce::exactlyEqual (lowPassFrequency, parameters[follower::lowPassFrequency]))
    {
        lowPassFrequency = parameters[follower::lowPassFrequency];
        lowPass.setCoefficients (juce::IIRCoefficients::makeLowPass (sampleRate, lowPassFrequency));
    }

    if (! juce::exactlyEqual (highPassFrequency, parameters[follower::highPassFrequency]))
    {
        highPassFrequency = parameters[follower::highPassFrequency];
        highPass.setCoefficients (juce::IIRCoefficients::makeHighPass (sampleRate, highPassFrequency));
    }

    for (int i = 0; i < numSamples; ++i)
    {
        auto level = 0.0f;

        for (int channel = 0; channel < numChannels; ++channel)
            level = std::max (level, std::abs (channels[channel][i]));

        level *= gain;

        if (lowPassOn)
            level = lowPass.processSingleSampleRaw (level);

        if (highPassOn)
            level = highPass.processSingleSampleRaw (level);

        if (level > envelope)
        {
            envelope = attack * (envelope - level) + level;
            holdLeft = holdSamples;
        }
        else if (holdLeft > 0)
        {
            --holdLeft;
        }
        else
        {
            envelope = release * (envelope - level) + level;
        }

        envelope = juce::jlimit (0.0f, 1.0f, envelope);
    }
}

double Modulator::movePhase (std::span<const float> parameters, double patternUnits, const SongPlace& place, double seconds) noexcept
{
    const auto timing = getTiming (kind);
    const auto sync = juce::roundToInt (parameters[(size_t) timing.syncType]);
    const auto rateType = juce::roundToInt (parameters[(size_t) timing.rateType]);
    const auto rate = std::max (0.01, (double) parameters[(size_t) timing.rate]);

    // Synced to the transport: where the song is, in seconds or in bars.
    if (sync == transport)
    {
        if (rateType == hertz)
        {
            const auto duration = patternUnits / rate;
            const auto at = std::fmod (std::max (0.0, place.seconds), duration) / duration;
            return at;
        }

        if (rateType >= fourBars && rateType <= sixtyFourthDotted)
        {
            const auto virtualBars = std::max (0.0, place.beats / place.numerator * rate / getBarFraction (rateType));
            return std::fmod (virtualBars, patternUnits) / patternUnits;
        }
    }

    // Free, or from the last note: a ramp that moves on by the time to the next control point.
    if (sync == note && noteStarted)
        ramp = 0.0;

    const auto duration =
        rateType == hertz ? patternUnits / rate : patternUnits * 60.0 / (place.tempo * rate / getBarFraction (rateType)) * place.numerator;
    const auto at = ramp;
    ramp += seconds / duration;
    ramp -= std::floor (ramp);
    return at;
}

float Modulator::getLfoValue (std::span<const float> parameters, float newPhase) noexcept
{
    newPhase = wrap (newPhase + parameters[lfo::phase]);

    // A cycle starting again draws the random waves' next value.
    if (newPhase < phase)
    {
        previousRandom = currentRandom;
        currentRandom = random.nextFloat();
    }

    phase = newPhase;
    const auto wave = (LfoWave) juce::jlimit (0, (int) LfoWave::noise, juce::roundToInt (parameters[lfo::wave]));
    auto value = wave == LfoWave::random  ? currentRandom
                 : wave == LfoWave::noise ? (currentRandom - previousRandom) * newPhase + previousRandom
                                          : getWaveValue (wave, newPhase);
    value = value * parameters[lfo::depth] + parameters[lfo::offset];
    return parameters[lfo::bipolar] >= 0.5f ? value * 2.0f - 1.0f : value;
}

float Modulator::getRandomValue (std::span<const float> parameters, float newPhase) noexcept
{
    // Smoothed, the cycle eases in and out (an S-curve).
    const auto smooth = parameters[randomSource::smooth];
    const auto eased = (1.0f - newPhase) * (1.0f - std::cos (newPhase * juce::MathConstants<float>::halfPi))
                       + newPhase * std::sin (newPhase * juce::MathConstants<float>::halfPi);
    newPhase = wrap ((1.0f - smooth) * newPhase + smooth * eased);

    // A cycle starting again draws the next value, at most its step depth from the last.
    const auto bipolar = parameters[randomSource::bipolar] >= 0.5f;

    if (newPhase < phase)
    {
        previousRandom = currentRandom;
        const auto stepDepth = parameters[randomSource::stepDepth] * (bipolar ? 1.0f : 0.5f);
        const auto low = std::max (currentRandom - stepDepth, bipolar ? -1.0f : 0.0f);
        const auto high = std::min (currentRandom + stepDepth, 1.0f);
        currentRandom = juce::jmap (random.nextFloat(), low, std::max (low, high));
    }

    phase = newPhase;

    // Held, then over the end of the cycle as long as its shape says, a line to the next value.
    const auto shape = parameters[randomSource::shape];
    const auto value = newPhase < 1.0f - shape || shape <= 0.0f
                           ? previousRandom
                           : (currentRandom - previousRandom) * ((newPhase - 1.0f) / shape + 1.0f) + previousRandom;
    return value * parameters[randomSource::depth];
}

float Modulator::getBreakpointValue (std::span<const float> parameters, float newPhase) const noexcept
{
    // Its stages: the first at the cycle's start, then up to four, each with its time, value and how its line bends.
    struct Stage
    {
        float value, time, curve;
    };

    std::array<Stage, 5> stages;
    stages[0] = { parameters[breakpoints::stageZeroValue], 0.0f, 0.0f };

    for (size_t i = 1; i < stages.size(); ++i)
    {
        const auto first = (size_t) breakpoints::stageZeroValue + 1 + (i - 1) * 3;
        stages[i] = { parameters[first], parameters[first + 1], parameters[first + 2] };
    }

    const auto active = (size_t) juce::jlimit (1, 4, juce::roundToInt (parameters[breakpoints::numActivePoints]));
    auto from = stages[active], to = stages[active];
    to.time = 1.0f;

    for (size_t i = 1; i <= active; ++i)
    {
        if (stages[i].time > newPhase)
        {
            from = stages[i - 1];
            to = stages[i];
            break;
        }
    }

    const auto value = juce::exactlyEqual (to.curve, 0.0f)
                           ? (juce::exactlyEqual (from.time, to.time)
                                  ? from.value
                                  : (from.value * (to.time - newPhase) + to.value * (newPhase - from.time)) / (to.time - from.time))
                           : getBentValue (newPhase, from.time, from.value, to.time, to.value, to.curve);
    const auto scaled = value * parameters[breakpoints::depth];
    return parameters[breakpoints::bipolar] >= 0.5f ? scaled * 2.0f - 1.0f : scaled;
}

float Modulator::next (std::span<const float> parameters, std::span<const float> stepValues, const SongPlace& place,
                       double seconds) noexcept
{
    auto value = 0.0f;

    switch (kind)
    {
        case ModulatorKind::lfo:
            value = getLfoValue (parameters, (float) movePhase (parameters, 1.0, place, seconds));
            break;

        case ModulatorKind::random:
            value = getRandomValue (parameters, (float) movePhase (parameters, 1.0, place, seconds));
            break;

        case ModulatorKind::breakpoints:
            value = getBreakpointValue (parameters, (float) movePhase (parameters, 1.0, place, seconds));
            break;

        case ModulatorKind::steps:
        {
            const auto numSteps = juce::jlimit (2, maxSteps, juce::roundToInt (parameters[steps::numSteps]));
            const auto at = movePhase (parameters, numSteps, place, seconds);
            const auto step = juce::jlimit (0, numSteps - 1, (int) std::floor (at * numSteps));
            const auto stepValue = (size_t) step < stepValues.size() ? juce::jlimit (-1.0f, 1.0f, stepValues[(size_t) step]) : 0.0f;
            value = stepValue * parameters[steps::depth];
            break;
        }

        case ModulatorKind::envelopeFollower:
            value = envelope * parameters[follower::depth] + parameters[follower::offset];
            break;

        case ModulatorKind::noteFollower:
        {
            // The last note's pitch or velocity: relative to a root, a spread above it reaching 1; or absolute, along a line
            // through Tracktion's five nodes, from -1 at 0 to 1 at 127.
            if (const auto midi = parameters[noteFollower::type] >= 0.5f ? lastVelocity : lastNote; lastNote >= 0)
            {
                if (parameters[noteFollower::mode] >= 0.5f)
                {
                    const auto root = parameters[noteFollower::relativeRoot];
                    const auto spread = std::max (1.0f, parameters[noteFollower::relativeSpread]);
                    noteValue = ((float) midi - root) / spread;
                }
                else
                {
                    constexpr int nodeNotes[] = { 0, 31, 63, 95, 127 };
                    auto node = 3;

                    while (node > 0 && nodeNotes[node] > midi)
                        --node;

                    const auto from = (float) nodeNotes[node], to = (float) nodeNotes[node + 1];
                    const auto fromValue = -1.0f + 0.5f * (float) node;
                    noteValue = fromValue + 0.5f * ((float) midi - from) / (to - from);
                }
            }

            value = noteValue;
            break;
        }
    }

    noteStarted = false;
    return value;
}
} // namespace phazite::devices
