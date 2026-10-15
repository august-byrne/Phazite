// SPDX-License-Identifier: AGPL-3.0-or-later
// Random numbers for DSP that come out the same every time from the same seed (Marsaglia's xorshift32), so that a
// device renders the same twice in a row and golden render tests can cover it (roadmap step 1.6). Real-time safe.
#pragma once

#include <cstdint>

namespace phazite::devices::dsp
{
class Random
{
public:
    explicit Random (std::uint32_t seed = defaultSeed) noexcept { setSeed (seed); }

    void setSeed (std::uint32_t seed) noexcept { state = seed != 0 ? seed : defaultSeed; }

    std::uint32_t nextInt() noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    // In [0, 1).
    float nextUnipolar() noexcept { return (float) (nextInt() >> 8) * (1.0f / 16777216.0f); }

    // In [-1, 1).
    float nextBipolar() noexcept { return 2.0f * nextUnipolar() - 1.0f; }

private:
    static constexpr std::uint32_t defaultSeed = 0x9e3779b9u;
    std::uint32_t state = defaultSeed;
};
} // namespace phazite::devices::dsp
