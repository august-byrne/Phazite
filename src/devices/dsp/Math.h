// SPDX-License-Identifier: AGPL-3.0-or-later
// Small helpers for Phazite's devices' DSP. Real-time safe.
#pragma once

#include <cmath>

namespace phazite::devices::dsp
{
// Decibels to a gain and back; at or below -120 dB the gain is 0, and a gain of 0 is -120 dB.
inline float decibelsToGain (float decibels) noexcept
{
    return decibels <= -120.0f ? 0.0f : std::pow (10.0f, decibels * 0.05f);
}

inline float gainToDecibels (float gain) noexcept
{
    return gain <= 1.0e-6f ? -120.0f : 20.0f * std::log10 (gain);
}

// A smooth saturation that follows tanh closely and reaches exactly 1 at 3 (a Pade approximant): cheap enough for
// every sample of every voice.
inline float softClip (float x) noexcept
{
    if (x >= 3.0f)
        return 1.0f;

    if (x <= -3.0f)
        return -1.0f;

    const auto x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// A note number's frequency in hertz, in equal temperament with A4 (note 69) at 440 Hz; fractional notes bend.
inline double noteToFrequency (double note) noexcept
{
    return 440.0 * std::pow (2.0, (note - 69.0) / 12.0);
}
} // namespace phazite::devices::dsp
