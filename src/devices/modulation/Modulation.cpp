// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Modulation.h"

#include <cmath>

namespace phazite::devices
{
namespace
{
    // A rising staircase of so many steps over the cycle, from 0 to 1.
    float getStepsUp (float phase, int steps)
    {
        const auto step = juce::jlimit (0, steps - 1, (int) std::floor (phase * (float) steps));
        return (float) step / (float) (steps - 1);
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
            return getStepsUp (phase, 4);
        case LfoWave::fourStepsDown:
            return getStepsUp (1.0f - phase, 4);
        case LfoWave::eightStepsUp:
            return getStepsUp (phase, 8);
        case LfoWave::eightStepsDown:
            return getStepsUp (1.0f - phase, 8);
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
