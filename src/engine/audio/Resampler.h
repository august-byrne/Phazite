// SPDX-License-Identifier: AGPL-3.0-or-later
// A stream of samples from one rate to another, through r8brain-free-src (MIT, external/r8brain-free-src; "Sample rate
// converter designed by Aleksey Vaneev of Voxengo"): a linear-phase windowed-sinc low-pass set by the ratio, so that the
// top octave keeps its level up to 0.95 of the lower rate's Nyquist frequency, and what lies above it is gone, 136 dB
// down, rather than folded back as aliasing. It takes the samples it is given and gives out as many as are ready, the
// first of them at the first sample given (it hides its own latency). An audio region plays a file at another rate than
// the engine's through one for each channel (AudioRegion.h).
#pragma once

#include <memory>

namespace r8b
{
class CDSPResampler;
}

namespace phazite::engine
{
class Resampler
{
public:
    // From a rate to another, given at most maximumInput samples at a time. Allocates (and the first of each pair of rates
    // works out its filters): off the audio thread.
    Resampler (double fromRate, double toRate, int maximumInput);
    ~Resampler();

    Resampler (const Resampler&) = delete;
    Resampler& operator= (const Resampler&) = delete;

    double getFromRate() const noexcept { return fromRate; }
    double getToRate() const noexcept { return toRate; }
    int getMaximumInput() const noexcept { return maximumInput; }

    // The most samples one call of process() gives out.
    int getMaximumOutput() const noexcept;

    // Real-time safe: forgets what it was given, as if made anew.
    void reset() noexcept;

    // Real-time safe: takes numInput samples (at most the maximum), and points output at the samples it gives out, which
    // it owns until the next call; returns how many. After a reset, the first comes out once the filter has taken enough.
    int process (const double* input, int numInput, const double*& output) noexcept;

private:
    double fromRate = 0.0, toRate = 0.0;
    int maximumInput = 0;
    std::unique_ptr<r8b::CDSPResampler> resampler;
};
} // namespace phazite::engine
