// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SpectralLoop.h"
#include "Random.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>

namespace phazite::devices::dsp
{
std::vector<float> makeSpectralLoop (const SpectralLoop& loop)
{
    const auto order = std::clamp (loop.order, 8, 22);
    const auto size = 1 << order;
    const auto half = size / 2;
    const auto binHertz = loop.sampleRate / size;

    // Each frequency's power: every harmonic's, spread over its band. Where bands overlap their powers add, as unrelated
    // players' would (the published pseudo-code adds their levels, which makes overlapping bands louder than their
    // harmonics are); and a harmonic's band always sums to the harmonic's own power, whatever its width.
    std::vector<float> power ((size_t) half, 0.0f);
    const auto firstWidth = (std::exp2 (loop.bandwidth / 1200.0) - 1.0) * loop.fundamental;
    constexpr double furthest = 14.71280603; // of x^2: past it a band's level is under -127 dB, and left out, as Nasca's is

    for (size_t index = 0; index < loop.harmonics.size(); ++index)
    {
        const auto number = (double) (index + 1);
        const auto centre = number * loop.fundamental;

        if (centre >= 0.5 * loop.sampleRate)
            break;

        const auto level = (double) loop.harmonics[index];

        if (! (level > 0.0))
            continue;

        // Half its bandwidth, in hertz: at least half the gap between two of the loop's frequencies, so that a band too
        // narrow for the loop to hold is its nearest frequency or two.
        const auto halfWidth = std::max (0.5 * firstWidth * std::pow (number, loop.bandwidthScale), 0.5 * binHertz);
        const auto reach = halfWidth * std::sqrt (furthest);
        const auto from = std::max (1, (int) std::ceil ((centre - reach) / binHertz));
        const auto to = std::min (half - 1, (int) std::floor ((centre + reach) / binHertz));

        const auto weightAt = [=] (int bin)
        {
            const auto x = (bin * binHertz - centre) / halfWidth;
            return std::exp (-2.0 * x * x); // the square of exp (-x^2): the band's power there
        };

        auto sum = 0.0;

        for (auto bin = from; bin <= to; ++bin)
            sum += weightAt (bin);

        if (! (sum > 0.0))
            continue;

        for (auto bin = from; bin <= to; ++bin)
            power[(size_t) bin] += (float) (level * level * weightAt (bin) / sum);
    }

    // A random phase for every frequency (drawn for each, whatever its level, so that a sound keeps its phases as its
    // spectrum is changed), nothing at 0 Hz or at half the rate, and the inverse transform of that.
    std::vector<float> data ((size_t) size * 2, 0.0f);
    Random random (loop.seed);

    for (int bin = 1; bin < half; ++bin)
    {
        const auto phase = juce::MathConstants<double>::twoPi * (double) random.nextUnipolar();
        const auto level = std::sqrt ((double) power[(size_t) bin]);
        data[(size_t) bin * 2] = (float) (level * std::cos (phase));
        data[(size_t) bin * 2 + 1] = (float) (level * std::sin (phase));
    }

    const juce::dsp::FFT fft (order);
    fft.performRealOnlyInverseTransform (data.data());
    data.resize ((size_t) size);

    auto energy = 0.0;

    for (const auto sample : data)
        energy += (double) sample * sample;

    if (const auto rms = std::sqrt (energy / size); rms > 0.0)
    {
        const auto gain = (float) (1.0 / rms);

        for (auto& sample : data)
            sample *= gain;
    }

    return data;
}
} // namespace phazite::devices::dsp
