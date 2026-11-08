// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SincReader.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace phazite::devices::dsp
{
namespace
{
    constexpr double kaiserBeta = 6.8; // about 70 dB down in the stopband

    // The modified Bessel function of the first kind, order 0, by its series.
    double besselI0 (double x)
    {
        auto sum = 1.0, term = 1.0;

        for (int k = 1; k < 50 && term > 1.0e-12 * sum; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }

        return sum;
    }

    const std::vector<float>& getKernel()
    {
        static const std::vector<float> kernel = []
        {
            constexpr auto count = SincReader::zeroCrossings * SincReader::resolution;
            std::vector<float> made ((size_t) count + 2, 0.0f);
            const auto norm = besselI0 (kaiserBeta);

            for (int i = 0; i <= count; ++i)
            {
                const auto x = (double) i / SincReader::resolution;
                const auto sinc = i == 0 ? 1.0 : std::sin (juce::MathConstants<double>::pi * x) / (juce::MathConstants<double>::pi * x);
                const auto along = x / SincReader::zeroCrossings;
                made[(size_t) i] = (float) (sinc * besselI0 (kaiserBeta * std::sqrt (std::max (0.0, 1.0 - along * along))) / norm);
            }

            return made;
        }();

        return kernel;
    }
} // namespace

SincReader::SincReader()
    : kernel (&getKernel())
{
}

void SincReader::read (const float* first, const float* second, int length, double position, double step, float& left,
                       float& right) const noexcept
{
    // The cutoff, as a share of the sound's Nyquist frequency: all of it, unless the sound is read faster than its rate.
    const auto cutoff = step <= 1.0 ? 1.0f : std::max (0.125f, (float) (1.0 / step));
    const auto reach = (int) std::ceil ((float) zeroCrossings / cutoff);
    const auto centre = (int) std::floor (position);
    const auto fraction = (float) (position - centre);
    const auto* points = kernel->data();
    auto sumLeft = 0.0f, sumRight = 0.0f;

    for (int n = std::max (centre - reach + 1, 0); n <= std::min (centre + reach, length - 1); ++n)
    {
        const auto place = std::abs ((float) (n - centre) - fraction) * cutoff * (float) resolution;

        if (place >= (float) (zeroCrossings * resolution))
            continue;

        const auto index = (int) place;
        const auto t = place - (float) index;
        const auto weight = points[index] + t * (points[index + 1] - points[index]);
        sumLeft += weight * first[n];
        sumRight += weight * second[n];
    }

    left = sumLeft * cutoff;
    right = sumRight * cutoff;
}

float SincReader::read (const float* samples, int length, double position, double step) const noexcept
{
    const auto cutoff = step <= 1.0 ? 1.0f : std::max (0.125f, (float) (1.0 / step));
    const auto reach = (int) std::ceil ((float) zeroCrossings / cutoff);
    const auto centre = (int) std::floor (position);
    const auto fraction = (float) (position - centre);
    const auto* points = kernel->data();
    auto sum = 0.0f;

    for (int n = std::max (centre - reach + 1, 0); n <= std::min (centre + reach, length - 1); ++n)
    {
        const auto place = std::abs ((float) (n - centre) - fraction) * cutoff * (float) resolution;

        if (place >= (float) (zeroCrossings * resolution))
            continue;

        const auto index = (int) place;
        const auto t = place - (float) index;
        sum += (points[index] + t * (points[index + 1] - points[index])) * samples[n];
    }

    return sum * cutoff;
}

void SincReader::halve (const float* samples, int length, float* halved)
{
    // read() at a step of 2: the kernel at half its band, its points at each sample away from the middle every half zero
    // crossing, of which those at a whole zero crossing are zero; the others, the same either side, at half weight.
    const auto& points = getKernel();
    std::array<float, zeroCrossings> odd {};

    for (int j = 0; j < zeroCrossings; ++j)
        odd[(size_t) j] = 0.5f * points[(size_t) ((2 * j + 1) * resolution / 2)];

    for (int i = 0; i < (length + 1) / 2; ++i)
    {
        const auto centre = 2 * i;
        auto sum = 0.5f * points[0] * samples[centre];

        for (int j = 0; j < zeroCrossings; ++j)
        {
            const auto offset = 2 * j + 1;
            const auto before = centre - offset >= 0 ? samples[centre - offset] : 0.0f;
            const auto after = centre + offset < length ? samples[centre + offset] : 0.0f;
            sum += odd[(size_t) j] * (before + after);
        }

        halved[i] = sum;
    }
}
} // namespace phazite::devices::dsp
