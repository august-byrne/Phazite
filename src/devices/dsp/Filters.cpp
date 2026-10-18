// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Filters.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <complex>

namespace phazite::devices::dsp
{
// The section's analog prototype, with its cutoff at 1 rad/s, is
//     H(s) = (m0 (s^2 + k s + 1) + m1 s + m2) / (s^2 + k s + 1)
// and the trapezoidal rule maps it to the digital filter with s = (z - 1) / (g (z + 1)), g = tan (pi f / fs): the
// bilinear transform, prewarped to the cutoff. The shapes' mixes and the gain's place in g and k follow Andrew
// Simper's published derivation of this filter (Cytomic, "Solving the continuous SVF equations using trapezoidal
// integration and equivalent currents", 2013).
FilterCoefficients makeFilter (FilterShape shape, double frequency, double sampleRate, double q, double gainDecibels)
{
    // tan() grows without bound at the Nyquist frequency.
    frequency = std::clamp (frequency, 1.0, 0.49 * sampleRate);
    q = std::max (q, 0.025);
    const auto a = std::pow (10.0, gainDecibels / 40.0); // the square root of the gain

    auto g = std::tan (juce::MathConstants<double>::pi * frequency / sampleRate);
    auto k = 1.0 / q;
    double m0 = 0.0, m1 = 0.0, m2 = 0.0;

    switch (shape)
    {
        case FilterShape::lowPass:
            m2 = 1.0;
            break;

        case FilterShape::highPass:
            m0 = 1.0;
            m1 = -k;
            m2 = -1.0;
            break;

        case FilterShape::bandPass:
            m1 = k;
            break;

        case FilterShape::notch:
            m0 = 1.0;
            m1 = -k;
            break;

        case FilterShape::allPass:
            m0 = 1.0;
            m1 = -2.0 * k;
            break;

        case FilterShape::bell:
            k = 1.0 / (q * a);
            m0 = 1.0;
            m1 = k * (a * a - 1.0);
            break;

        case FilterShape::lowShelf:
            g /= std::sqrt (a);
            m0 = 1.0;
            m1 = k * (a - 1.0);
            m2 = a * a - 1.0;
            break;

        case FilterShape::highShelf:
            g *= std::sqrt (a);
            m0 = a * a;
            m1 = k * (1.0 - a) * a;
            m2 = 1.0 - a * a;
            break;
    }

    const auto a1 = 1.0 / (1.0 + g * (g + k));
    const auto a2 = g * a1;
    const auto a3 = g * a2;
    return { (float) g, (float) k, (float) a1, (float) a2, (float) a3, (float) m0, (float) m1, (float) m2 };
}

double getMagnitude (const FilterCoefficients& c, double frequency, double sampleRate)
{
    return std::abs (getResponse (c, frequency, sampleRate));
}

std::complex<double> getResponse (const FilterCoefficients& c, double frequency, double sampleRate)
{
    frequency = std::clamp (frequency, 0.0, 0.4999 * sampleRate);
    const auto w = std::tan (juce::MathConstants<double>::pi * frequency / sampleRate) / (double) c.g;
    const std::complex<double> s (0.0, w);
    const auto denominator = s * s + (double) c.k * s + 1.0;
    const auto numerator = (double) c.m0 * denominator + (double) c.m1 * s + (double) c.m2;
    return numerator / denominator;
}

double getButterworthQ (int order, int section)
{
    const auto angle = juce::MathConstants<double>::pi * (2.0 * section + 1.0) / (2.0 * order);
    return 1.0 / (2.0 * std::cos (angle));
}
} // namespace phazite::devices::dsp
