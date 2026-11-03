// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Modulation.h"

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
namespace
{
    // A staircase of so many steps over the cycle, from 0 to 1, going up or down: the step the point is on, as a part of the
    // highest.
    float getStairs (float phase, int steps, bool up)
    {
        const auto step = std::clamp ((int) (phase * (float) steps), 0, steps - 1);
        return (float) (up ? step : steps - 1 - step) / (float) (steps - 1);
    }
} // namespace

bool isPeriodic (LfoWave wave)
{
    return wave != LfoWave::random && wave != LfoWave::noise;
}

float getWaveValue (LfoWave wave, float phase)
{
    switch (wave)
    {
        case LfoWave::sine:
            return (std::sin (phase * juce::MathConstants<float>::twoPi) + 1.0f) / 2.0f;
        case LfoWave::triangle:
            return phase < 0.5f ? 2.0f * phase : 2.0f - 2.0f * phase;
        case LfoWave::sawUp:
            return phase;
        case LfoWave::sawDown:
            return 1.0f - phase;
        case LfoWave::square:
            return phase < 0.5f ? 1.0f : 0.0f;
        case LfoWave::fourStepsUp:
            return getStairs (phase, 4, true);
        case LfoWave::fourStepsDown:
            return getStairs (phase, 4, false);
        case LfoWave::eightStepsUp:
            return getStairs (phase, 8, true);
        case LfoWave::eightStepsDown:
            return getStairs (phase, 8, false);
        case LfoWave::random:
        case LfoWave::noise:
            break;
    }

    return 0.0f;
}

float getLfoValue (const LfoShape& lfo, float phase)
{
    const auto value = getWaveValue (lfo.wave, phase) * lfo.depth + lfo.offset;
    return lfo.bipolar ? value * 2.0f - 1.0f : value;
}

float movePhase (float phase, double cycles)
{
    const auto moved = std::fmod ((double) phase + cycles, 1.0);
    return (float) (moved < 0.0 ? moved + 1.0 : moved);
}

float modulate (const ParameterInfo& parameter, const juce::NormalisableRange<float>& range, float value, float offset)
{
    if (juce::exactlyEqual (offset, 0.0f))
        return constrain (parameter, value);

    const auto moved = juce::jlimit (0.0f, 1.0f, range.convertTo0to1 (range.getRange().clipValue (value)) + offset);
    return constrain (parameter, range.convertFrom0to1 (moved));
}
} // namespace phazite::devices
