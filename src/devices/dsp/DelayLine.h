// SPDX-License-Identifier: AGPL-3.0-or-later
// A delay line for Phazite's devices (the reverb's, a limiter's look-ahead): a ring of samples whose length is a power
// of two, so that wrapping around is a mask. prepare() allocates; everything else is real-time safe.
#pragma once

#include <cmath>
#include <vector>

namespace phazite::devices::dsp
{
class DelayLine
{
public:
    // Room for delays up to maximumDelay samples. Allocates: call it before playback.
    void prepare (int maximumDelay)
    {
        int size = 1;

        while (size < maximumDelay + 3) // the interpolating read reaches two samples further
            size *= 2;

        buffer.assign ((size_t) size, 0.0f);
        mask = size - 1;
        writeIndex = 0;
    }

    void reset() noexcept
    {
        for (auto& sample : buffer)
            sample = 0.0f;
    }

    void push (float sample) noexcept
    {
        writeIndex = (writeIndex + 1) & mask;
        buffer[(size_t) writeIndex] = sample;
    }

    // A sample pushed before: 0 is the one pushed last, 1 the one before it, and so on, up to the maximum delay.
    float read (int delay) const noexcept { return buffer[(size_t) ((writeIndex - delay) & mask)]; }

    // Between samples, by 4-point Lagrange interpolation, as the comb filter reads (CombFilter.h): for a delay that moves
    // (a modulated reverb line's, a delay's gliding), or that isn't a whole number of samples, so that an echo that goes
    // round and round keeps its treble (a straight line between two samples dulls it a little more each time). At a whole
    // number of samples it is that sample, exactly. Under a sample it reads linearly: the newest sample has none after it.
    float read (float delay) const noexcept
    {
        const auto whole = (int) std::floor (delay);
        const auto f = delay - (float) whole;

        if (whole < 1)
        {
            const auto first = read (whole), second = read (whole + 1);
            return first + f * (second - first);
        }

        const auto a = read (whole - 1), b = read (whole), c = read (whole + 1), d = read (whole + 2);
        const auto fp1 = f + 1.0f, fm1 = f - 1.0f, fm2 = f - 2.0f;
        return (d * fp1 * f * fm1 - a * f * fm1 * fm2) * (1.0f / 6.0f) + (b * fp1 * fm1 * fm2 - c * fp1 * f * fm2) * 0.5f;
    }

private:
    std::vector<float> buffer { 0.0f, 0.0f };
    int mask = 1, writeIndex = 0;
};
} // namespace phazite::devices::dsp
