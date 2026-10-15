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

        while (size < maximumDelay + 2) // the interpolating read reaches one sample further
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

    // Between two samples, interpolating linearly: for a delay that moves, such as a modulated reverb line's.
    float read (float delay) const noexcept
    {
        const auto whole = (int) std::floor (delay);
        const auto fraction = delay - (float) whole;
        const auto a = read (whole), b = read (whole + 1);
        return a + fraction * (b - a);
    }

private:
    std::vector<float> buffer { 0.0f, 0.0f };
    int mask = 1, writeIndex = 0;
};
} // namespace phazite::devices::dsp
