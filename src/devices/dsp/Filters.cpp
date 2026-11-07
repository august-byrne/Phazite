// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Filters.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <complex>

namespace phazite::devices::dsp
{
namespace
{
    // The section's analog prototype, with its cutoff at 1 rad/s (times scale, which the shelves move):
    //     H(s) = (m0 (s^2 + k s + 1) + m1 s + m2) / (s^2 + k s + 1)
    // The shapes' mixes and the gain's place in the frequency and k follow Andrew Simper's published derivation of this
    // filter (Cytomic, "Solving the continuous SVF equations using trapezoidal integration and equivalent currents", 2013).
    struct Prototype
    {
        double k = 1.0, m0 = 0.0, m1 = 0.0, m2 = 0.0;
        double scale = 1.0; // where the prototype's 1 rad/s is, of the section's frequency
    };

    // a is the square root of the gain.
    Prototype getPrototype (FilterShape shape, double q, double a)
    {
        Prototype p;
        p.k = 1.0 / q;

        switch (shape)
        {
            case FilterShape::lowPass:
                p.m2 = 1.0;
                break;

            case FilterShape::highPass:
                p.m0 = 1.0;
                p.m1 = -p.k;
                p.m2 = -1.0;
                break;

            case FilterShape::bandPass:
                p.m1 = p.k;
                break;

            case FilterShape::notch:
                p.m0 = 1.0;
                p.m1 = -p.k;
                break;

            case FilterShape::allPass:
                p.m0 = 1.0;
                p.m1 = -2.0 * p.k;
                break;

            case FilterShape::bell:
                p.k = 1.0 / (q * a);
                p.m0 = 1.0;
                p.m1 = p.k * (a * a - 1.0);
                break;

            case FilterShape::lowShelf:
                p.scale = 1.0 / std::sqrt (a);
                p.m0 = 1.0;
                p.m1 = p.k * (a - 1.0);
                p.m2 = a * a - 1.0;
                break;

            case FilterShape::highShelf:
                p.scale = std::sqrt (a);
                p.m0 = a * a;
                p.m1 = p.k * (1.0 - a) * a;
                p.m2 = 1.0 - a * a;
                break;
        }

        return p;
    }

    std::complex<double> getPrototypeResponse (const Prototype& p, double w)
    {
        const std::complex<double> s (0.0, w);
        const auto denominator = s * s + p.k * s + 1.0;
        return (p.m0 * denominator + p.m1 * s + p.m2) / denominator;
    }

    FilterCoefficients fromSvf (double g, double k, double m0, double m1, double m2)
    {
        const auto a1 = 1.0 / (1.0 + g * (g + k));
        const auto a2 = g * a1;
        const auto a3 = g * a2;
        return { (float) g, (float) k, (float) a1, (float) a2, (float) a3, (float) m0, (float) m1, (float) m2 };
    }
} // namespace

// The trapezoidal rule maps the prototype to the digital filter with s = (z - 1) / (g (z + 1)), g = tan (pi f / fs): the
// bilinear transform, prewarped to the cutoff.
FilterCoefficients makeFilter (FilterShape shape, double frequency, double sampleRate, double q, double gainDecibels)
{
    // tan() grows without bound at the Nyquist frequency.
    frequency = std::clamp (frequency, 1.0, 0.49 * sampleRate);
    q = std::max (q, 0.025);
    const auto a = std::pow (10.0, gainDecibels / 40.0); // the square root of the gain
    const auto p = getPrototype (shape, q, a);
    auto g = std::tan (juce::MathConstants<double>::pi * frequency / sampleRate);

    if (shape == FilterShape::lowShelf)
        g /= std::sqrt (a);
    else if (shape == FilterShape::highShelf)
        g *= std::sqrt (a);

    return fromSvf (g, p.k, p.m0, p.m1, p.m2);
}

FilterCoefficients makeMatchedFilter (FilterShape shape, double frequency, double sampleRate, double q, double gainDecibels)
{
    if (shape == FilterShape::bandPass || shape == FilterShape::allPass)
        return makeFilter (shape, frequency, sampleRate, q, gainDecibels);

    constexpr auto pi = juce::MathConstants<double>::pi;
    frequency = std::clamp (frequency, 1.0, 0.49 * sampleRate);
    q = std::max (q, 0.025);
    const auto p = getPrototype (shape, q, std::pow (10.0, gainDecibels / 40.0));

    // The poles: the analog section's, at its frequency (unwarped), mapped by z = e^(s T). One that would lie above the
    // Nyquist frequency (a high shelf's, boosted near the top) is kept just below it rather than folded back.
    const auto unit = 2.0 * pi * frequency * p.scale / sampleRate; // the prototype's 1 rad/s, in radians a sample
    double a1 = 0.0, a2 = 0.0;

    if (p.k < 2.0)
    {
        const auto radius = std::exp (-0.5 * p.k * unit);
        const auto angle = std::min (unit * std::sqrt (1.0 - 0.25 * p.k * p.k), 0.95 * pi);
        a1 = -2.0 * radius * std::cos (angle);
        a2 = radius * radius;
    }
    else
    {
        const auto spread = std::sqrt (0.25 * p.k * p.k - 1.0);
        a1 = -(std::exp ((-0.5 * p.k + spread) * unit) + std::exp ((-0.5 * p.k - spread) * unit));
        a2 = std::exp (-p.k * unit);
    }

    // The zeros: the analog gain, squared, at 0 Hz, at the Nyquist frequency and at the section's frequency, matched with
    // |H|^2 = (B0 c + B1 s + B2 4 c s) / (A0 c + A1 s + A2 4 c s), where c and s are cos^2 and sin^2 of half the frequency
    // in radians a sample, A0 and B0 the squared sums of the coefficients, A1 and B1 those with the middle one negated,
    // and A2 and B2 -4 times the outer ones' products.
    const auto analogGain = [&] (double hertz) { return std::norm (getPrototypeResponse (p, hertz / (frequency * p.scale))); };
    const auto A0 = (1.0 + a1 + a2) * (1.0 + a1 + a2), A1 = (1.0 - a1 + a2) * (1.0 - a1 + a2), A2 = -4.0 * a2;
    const auto B0 = analogGain (0.0) * A0;
    const auto B1 = analogGain (0.5 * sampleRate) * A1;
    const auto half = pi * frequency / sampleRate;
    const auto c = std::cos (half) * std::cos (half), s = std::sin (half) * std::sin (half);
    const auto B2 = (analogGain (frequency) * (A0 * c + A1 * s + A2 * 4.0 * c * s) - B0 * c - B1 * s) / (4.0 * c * s);

    const auto rootB0 = std::sqrt (std::max (B0, 0.0)), rootB1 = std::sqrt (std::max (B1, 0.0));
    const auto w = 0.5 * (rootB0 + rootB1);
    const auto spread = std::sqrt (std::max (w * w + B2, 0.0));
    auto b0 = 0.5 * (w + spread), b1 = 0.5 * (rootB0 - rootB1), b2 = 0.5 * (w - spread);

    // A notch's zeros on the circle at its frequency, as the analog section's map to: silence there, its gain at 0 Hz matched.
    if (shape == FilterShape::notch)
    {
        const auto cosine = std::cos (2.0 * half);
        b0 = b2 = rootB0 / (2.0 - 2.0 * cosine);
        b1 = -2.0 * cosine * b0;
    }

    // The digital section as the state-variable filter's: the bilinear transform taken back, s = (z - 1) / (g (z + 1)),
    // with g chosen so that the poles' analog section is s^2 + k s + 1.
    const auto sum = 1.0 + a1 + a2;
    const auto g = std::sqrt (sum / (1.0 - a1 + a2));
    const auto k = 2.0 * g * (1.0 - a2) / sum;
    const auto n0 = (b0 + b1 + b2) / sum, n1 = 2.0 * g * (b0 - b2) / sum, n2 = g * g * (b0 - b1 + b2) / sum;
    return fromSvf (g, k, n2, n1 - n2 * k, n0 - n2);
}

std::complex<double> getAnalogResponse (FilterShape shape, double frequency, double cutoff, double q, double gainDecibels)
{
    const auto p = getPrototype (shape, std::max (q, 0.025), std::pow (10.0, gainDecibels / 40.0));
    return getPrototypeResponse (p, frequency / (cutoff * p.scale));
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
