// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Modulators.h"
#include "Modulation.h"
#include "devices/dsp/Math.h"

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
namespace
{
    // How a cycle is timed: on from where it was (free), where the song is (with the transport), or from each note.
    enum Sync
    {
        free,
        transport,
        note
    };

    // Its rate, in hertz (0) or in lengths of the bar: four bars, two, one, then each note value from a half to a
    // sixty-fourth as a triplet, straight and dotted (1 to 21).
    constexpr int inHertz = 0, longestLength = 1, shortestLength = 21;

    // A length's part of a bar.
    double getBarFraction (int rateType) noexcept
    {
        if (rateType < longestLength || rateType > shortestLength)
            return 1.0;

        if (rateType <= 3)
            return std::ldexp (1.0, 3 - rateType); // four bars, two, one

        constexpr double feels[] = { 2.0 / 3.0, 1.0, 1.5 }; // a triplet, straight, dotted
        const auto noteValue = (rateType - 4) / 3;          // a half note, then a quarter, down to a sixty-fourth
        return std::ldexp (1.0, -(noteValue + 1)) * feels[(rateType - 4) % 3];
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

    // Where a value is in its cycle, from 0 up to (not including) 1.
    double getFraction (double cycles) noexcept
    {
        return cycles - std::floor (cycles);
    }

    // Eased in and out: slow at both ends, as a smoothstep.
    float easeInOut (float along) noexcept
    {
        return along * along * (3.0f - 2.0f * along);
    }

    // A modulator's value from 0 to 1 as it puts it out: from -1 to 1 if it is bipolar.
    float toOutput (float value, bool bipolar) noexcept
    {
        return bipolar ? 2.0f * value - 1.0f : value;
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
    cycle = 0.0;
    lastPoint = 0.0f;
    fromRandom = toRandom = 0.0f;
    noteStarted = false;
    envelope = 0.0f;
    holdLeft = 0;
    lowPass.reset();
    highPass.reset();
    lastNote = -1;
    lastVelocity = 0;
    noteValue = -1.0f; // the lowest note's, until a note is heard
}

void Modulator::noteOn (int pitch, int velocity) noexcept
{
    noteStarted = true;
    lastNote = pitch;
    lastVelocity = velocity;
}

void Modulator::listen (const float* const* channels, int numChannels, int numSamples, std::span<const float> parameters) noexcept
{
    if (kind != ModulatorKind::envelopeFollower || numChannels <= 0)
        return;

    // The sound, its channels mixed, through its filters if they are on (a high-pass to leave out its lows, a low-pass its
    // highs), then its level by its gain. The envelope rises toward it at its attack and falls at its release, each a time
    // constant (as far as 1 - 1/e of the way in that time), and holds a peak for its hold first.
    const auto getStep = [this] (float milliseconds)
    { return 1.0f - std::exp (-1.0f / (std::max (milliseconds, 0.01f) * 0.001f * (float) sampleRate)); };
    const auto attackStep = getStep (parameters[follower::attack]);
    const auto releaseStep = getStep (parameters[follower::release]);
    const auto holdSamples = (int) std::lround (parameters[follower::hold] * 0.001 * sampleRate);
    const auto gain = dsp::decibelsToGain (parameters[follower::gainDb]);
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
        auto mixed = 0.0f;

        for (int channel = 0; channel < numChannels; ++channel)
            mixed += channels[channel][i];

        mixed /= (float) numChannels;

        if (highPassOn)
            mixed = highPass.processSingleSampleRaw (mixed);

        if (lowPassOn)
            mixed = lowPass.processSingleSampleRaw (mixed);

        const auto level = std::abs (mixed) * gain;

        if (level >= envelope)
        {
            envelope += (level - envelope) * attackStep;
            holdLeft = holdSamples;
        }
        else if (holdLeft > 0)
        {
            --holdLeft;
        }
        else
        {
            envelope += (level - envelope) * releaseStep;
        }

        envelope = std::clamp (envelope, 0.0f, 1.0f);
    }
}

double Modulator::movePhase (std::span<const float> parameters, double units, const SongPlace& place, double seconds) noexcept
{
    const auto timing = getTiming (kind);
    const auto sync = juce::roundToInt (parameters[(size_t) timing.syncType]);
    const auto rateType = juce::roundToInt (parameters[(size_t) timing.rateType]);
    const auto rate = std::max (0.01, (double) parameters[(size_t) timing.rate]);
    const auto inBars = rateType >= longestLength && rateType <= shortestLength;

    // With the transport: as many cycles in as the song is, from its start, in seconds or in bars.
    if (sync == transport)
    {
        const auto cycles = inBars ? std::max (0.0, place.beats) / place.numerator / getBarFraction (rateType) * rate
                                   : std::max (0.0, place.seconds) * rate;
        return getFraction (cycles / units);
    }

    // Free, or from the last note started: on from where it was, by the time to the next control point.
    if (sync == note && noteStarted)
        cycle = 0.0;

    const auto barSeconds = 60.0 / place.tempo * place.numerator;
    const auto cycleSeconds = units * (inBars ? getBarFraction (rateType) * barSeconds / rate : 1.0 / rate);
    const auto at = cycle;
    cycle = getFraction (cycle + seconds / cycleSeconds);
    return at;
}

float Modulator::getLfoValue (std::span<const float> parameters, float point) noexcept
{
    point = (float) getFraction ((double) point + parameters[lfo::phase]);

    // Each cycle that starts picks the random waves' next value: the random wave holds it for the cycle, and noise glides to
    // it over the cycle, eased in and out.
    if (point < lastPoint)
    {
        fromRandom = toRandom;
        toRandom = random.nextFloat();
    }

    lastPoint = point;
    const auto wave = (LfoWave) juce::jlimit (0, (int) LfoWave::noise, juce::roundToInt (parameters[lfo::wave]));
    const auto value = wave == LfoWave::random  ? toRandom
                       : wave == LfoWave::noise ? fromRandom + (toRandom - fromRandom) * easeInOut (point)
                                                : getWaveValue (wave, point);
    return toOutput (value * parameters[lfo::depth] + parameters[lfo::offset], parameters[lfo::bipolar] >= 0.5f);
}

float Modulator::getRandomValue (std::span<const float> parameters, float point) noexcept
{
    // Each cycle that starts picks a target: somewhere within its step depth of the last (a part of the whole range),
    // inside the range, from -1 or 0 to 1.
    const auto bipolar = parameters[randomSource::bipolar] >= 0.5f;
    const auto lowest = bipolar ? -1.0f : 0.0f;

    if (point < lastPoint)
    {
        fromRandom = toRandom;
        const auto reach = parameters[randomSource::stepDepth] * (1.0f - lowest);
        toRandom = std::clamp (fromRandom + (2.0f * random.nextFloat() - 1.0f) * reach, lowest, 1.0f);
    }

    lastPoint = point;

    // It holds the last target, then glides to the new one over the end of the cycle that its shape takes, in a line, or
    // eased in and out as far as it is smoothed.
    const auto glide = std::clamp (parameters[randomSource::shape], 0.0f, 1.0f);
    auto value = fromRandom;

    if (glide > 0.0f && point >= 1.0f - glide)
    {
        const auto along = std::clamp ((point - (1.0f - glide)) / glide, 0.0f, 1.0f);
        const auto smooth = std::clamp (parameters[randomSource::smooth], 0.0f, 1.0f);
        value = fromRandom + (toRandom - fromRandom) * ((1.0f - smooth) * along + smooth * easeInOut (along));
    }

    return value * parameters[randomSource::depth];
}

float Modulator::getBreakpointValue (std::span<const float> parameters, float point) const noexcept
{
    // Up to four stages after its start, each to its value by its time (a part of the cycle), its line bent by its curve
    // (from -0.5 to 0.5, half the ease of devices/dsp/Math.h); after the last, it holds that one's value.
    const auto count = juce::jlimit (1, 4, juce::roundToInt (parameters[breakpoints::numActivePoints]));
    auto startTime = 0.0f, value = parameters[breakpoints::stageZeroValue];

    for (int stage = 1; stage <= count; ++stage)
    {
        const auto first = (size_t) breakpoints::stageZeroValue + 1 + (size_t) (stage - 1) * 3;
        const auto endValue = parameters[first], endTime = parameters[first + 1], curve = parameters[first + 2];

        if (point < endTime)
        {
            const auto length = endTime - startTime;
            const auto along = length > 0.0f ? std::clamp ((point - startTime) / length, 0.0f, 1.0f) : 1.0f;
            const auto ease = std::clamp ((double) curve * 2.0, -1.0, 1.0);
            value += (endValue - value) * (float) dsp::getEasedProgress (along, ease);
            break;
        }

        startTime = endTime;
        value = endValue;
    }

    return toOutput (value * parameters[breakpoints::depth], parameters[breakpoints::bipolar] >= 0.5f);
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
            // Its pattern is its cycle, each step one length of its rate.
            const auto numSteps = juce::jlimit (2, maxSteps, juce::roundToInt (parameters[steps::numSteps]));
            const auto at = movePhase (parameters, numSteps, place, seconds);
            const auto step = std::min (numSteps - 1, (int) (at * numSteps));
            const auto stepValue = (size_t) step < stepValues.size() ? std::clamp (stepValues[(size_t) step], -1.0f, 1.0f) : 0.0f;
            value = stepValue * parameters[steps::depth];
            break;
        }

        case ModulatorKind::envelopeFollower:
            value = envelope * parameters[follower::depth] + parameters[follower::offset];
            break;

        case ModulatorKind::noteFollower:
        {
            // The last note's pitch or velocity: from a root, a spread above it reaching 1; or across the whole range, from
            // -1 at 0 to 1 at 127.
            if (const auto heard = parameters[noteFollower::type] >= 0.5f ? lastVelocity : lastNote; lastNote >= 0)
            {
                const auto relative = parameters[noteFollower::mode] >= 0.5f;
                noteValue = relative ? ((float) heard - parameters[noteFollower::relativeRoot])
                                           / std::max (1.0f, parameters[noteFollower::relativeSpread])
                                     : (float) heard / 63.5f - 1.0f;
            }

            value = noteValue;
            break;
        }
    }

    noteStarted = false;
    return value;
}
} // namespace phazite::devices
