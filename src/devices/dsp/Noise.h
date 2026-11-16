// SPDX-License-Identifier: AGPL-3.0-or-later
// White noise for Phazite's synths, alike at every sample rate. At a session's usual rates (44.1 and 48 kHz) and up to
// twice them, random samples as they are. Above, noise made at a half, a quarter or an eighth of the rate (44.1 or 48 kHz,
// as the rate is) and raised to it by the polyphase IIR half-band filters the drives use (Oversampling.h), so that nothing
// of it lies above the band noise at 44.1 or 48 kHz has. And at the rate it is made at, as loud for each hertz as random
// samples at 48 kHz are, the rate Phazite's presets are leveled at (docs/presets.md): there it is the random numbers
// themselves, as it always was, and at 96 and 192 kHz it is made from them at 48 kHz; at 44.1 kHz (and 88.2 and 176.4)
// the square root of 44.1 / 48 down, 0.37 dB, so that what each part of the band holds is the same at every rate. White
// noise at the device's own rate would spread over the whole band up to half the rate: at 96 kHz less than half of it
// heard; turned up to make up for that, its peaks would rise and a drive after it would saturate it harder. Made so, its
// level in the band that is heard, its peaks (as a converter rebuilds the samples) and what a drive does to it are those
// at 48 kHz. Its random numbers come from a seed, so a render repeats exactly. Real-time safe once made (the half-bands
// are designed as the first oversampler is made, off the audio thread).
#pragma once

#include "Oversampling.h"
#include "Random.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace phazite::devices::dsp
{
class Noise
{
public:
    void prepare (double sampleRate) noexcept
    {
        // Halved until it is under twice 44.1 kHz.
        stages = 0;
        auto rate = sampleRate;

        for (; rate >= 2.0 * 44100.0 * 0.999 && stages < maxStages; rate *= 0.5)
            ++stages;

        gain = (float) std::sqrt (rate / referenceRate); // exactly 1 at 48 kHz and its multiples
        restart();
    }

    void setSeed (std::uint32_t seed) noexcept
    {
        random.setSeed (seed);
        restart();
    }

    // In [-1, 1) at 48 kHz; about as much at the other rates.
    float next() noexcept
    {
        if (stages == 0)
            return gain * random.nextBipolar();

        if (taken == ready)
            makeMore();

        return made[(size_t) taken++];
    }

private:
    static constexpr int maxStages = 3;              // up to eight times 44.1 or 48 kHz
    static constexpr double referenceRate = 48000.0; // where it is as loud for each hertz as it always was

    Random random;
    int stages = 0;
    float gain = 1.0f;
    std::array<Oversampler2x, maxStages> raisers;
    std::array<float, (size_t) (1 << maxStages)> made {};
    int ready = 0, taken = 0;

    // A sample at the rate it is made at, raised through each stage to as many at the device's.
    void makeMore() noexcept
    {
        std::array<float, (size_t) (1 << maxStages)> raised {};
        made[0] = gain * random.nextBipolar();
        auto count = 1;

        for (int stage = 0; stage < stages; ++stage)
        {
            for (int i = 0; i < count; ++i)
            {
                const auto [first, second] = raisers[(size_t) stage].up (made[(size_t) i]);
                raised[(size_t) (2 * i)] = first;
                raised[(size_t) (2 * i + 1)] = second;
            }

            count *= 2;
            made = raised;
        }

        ready = count;
        taken = 0;
    }

    void restart() noexcept
    {
        for (auto& raiser : raisers)
            raiser.reset();

        ready = taken = 0;
    }
};
} // namespace phazite::devices::dsp
