// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Click.h"

#include <cmath>

namespace phazite::engine
{
juce::AudioBuffer<float> makeClick (double sampleRate, bool accent)
{
    // Two partials a little out of tune with each other, as a woodblock's are, dying away in a few milliseconds, after
    // an attack of half a millisecond so that it doesn't start with a step.
    // Peaks near -4.5 and -7 dBFS, a bar's first clearly louder.
    constexpr double seconds = 0.05, attack = 0.0005;
    const auto decay = accent ? 0.012 : 0.008;
    const auto frequency = accent ? 1760.0 : 1320.0;
    const auto level = accent ? 0.72 : 0.5;
    const auto length = (int) std::ceil (seconds * sampleRate);

    juce::AudioBuffer<float> click (1, length);
    auto* samples = click.getWritePointer (0);

    for (int i = 0; i < length; ++i)
    {
        const auto t = i / sampleRate;
        const auto envelope = std::min (1.0, t / attack) * std::exp (-t / decay);
        const auto tone = 0.7 * std::sin (juce::MathConstants<double>::twoPi * frequency * t)
                          + 0.3 * std::sin (juce::MathConstants<double>::twoPi * frequency * 2.37 * t);
        samples[i] = (float) (level * envelope * tone);
    }

    return click;
}
} // namespace phazite::engine
