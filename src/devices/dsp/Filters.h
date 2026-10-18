// SPDX-License-Identifier: AGPL-3.0-or-later
// Filters for Phazite's devices: a state-variable filter integrated with the trapezoidal rule (the "topology-preserving
// transform"), which stays stable and smooth while its frequency moves fast, so it serves the synth's filter as well
// as the EQ's bands. One set of coefficients gives every shape; the filter is exactly the bilinear transform of an
// analog second-order section, so getMagnitude() gives its response exactly, for drawing an EQ's curve and for tests.
#pragma once

#include <cmath>
#include <complex>

namespace phazite::devices::dsp
{
enum class FilterShape
{
    lowPass,
    highPass,
    bandPass, // unity gain at the centre
    notch,
    bell,      // boosts or cuts around the frequency
    lowShelf,  // boosts or cuts below the frequency
    highShelf, // boosts or cuts above the frequency
    allPass
};

struct FilterCoefficients
{
    // The integrators' gain and the damping (1 / Q), then the mix of input, band and low outputs.
    float g = 0.0f, k = 1.4142135f;
    float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f;
    float m0 = 1.0f, m1 = 0.0f, m2 = 0.0f; // passes the input through until set
};

// The coefficients for a shape at a frequency (kept below the Nyquist frequency) and Q; gainDecibels counts for the
// bell and the shelves only.
FilterCoefficients makeFilter (FilterShape, double frequency, double sampleRate, double q, double gainDecibels = 0.0);

// The filter's gain (not in decibels) at a frequency; and its response there, its gain and phase, for filters whose
// sections are added together.
double getMagnitude (const FilterCoefficients&, double frequency, double sampleRate);
std::complex<double> getResponse (const FilterCoefficients&, double frequency, double sampleRate);

// The Q of each second-order section of a Butterworth filter of an even order (2, 4, 6 or 8), whose sections in a
// row give a maximally flat slope of 6 dB per octave per order, -3 dB at the cutoff. Section must be below order / 2.
double getButterworthQ (int order, int section);

//==============================================================================
// One channel of the filter. Real-time safe; reset() it when the sound it holds should go.
class Filter
{
public:
    void reset() noexcept { ic1 = ic2 = 0.0f; }

    float process (float input, const FilterCoefficients& c) noexcept
    {
        const auto v3 = input - ic2;
        const auto v1 = c.a1 * ic1 + c.a2 * v3;
        const auto v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return c.m0 * input + c.m1 * v1 + c.m2 * v2;
    }

    // What the next sample's output will be, before process() takes its input: gain * input + offset. For filters inside
    // a feedback loop, so that the loop is solved at once rather than a sample late.
    struct Instant
    {
        float gain, offset;
    };

    Instant getInstant (const FilterCoefficients& c) const noexcept
    {
        return { c.m0 + c.m1 * c.a2 + c.m2 * c.a3, c.m1 * (c.a1 * ic1 - c.a2 * ic2) + c.m2 * (ic2 + c.a2 * ic1 - c.a3 * ic2) };
    }

    // The low-pass, band-pass and high-pass outputs at once, for a filter whose shape moves between them.
    struct Outputs
    {
        float low, band, high;
    };

    Outputs processAll (float input, const FilterCoefficients& c) noexcept
    {
        const auto v3 = input - ic2;
        const auto v1 = c.a1 * ic1 + c.a2 * v3;
        const auto v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return { v2, v1, input - c.k * v1 - v2 };
    }

private:
    float ic1 = 0.0f, ic2 = 0.0f;
};
} // namespace phazite::devices::dsp
