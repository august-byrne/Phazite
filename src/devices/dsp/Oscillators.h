// SPDX-License-Identifier: AGPL-3.0-or-later
// Oscillators for Phazite's devices: a band-limited one for the synth's voices, whose saw and square waves have their
// steps smoothed with PolyBLEP (a polynomial band-limited step) so that high notes don't alias into harsh tones, and
// a low-frequency one for modulation. Real-time safe.
#pragma once

#include "Random.h"

#include <cmath>

namespace phazite::devices::dsp
{
// sin (2 pi phase), for a phase from 0 to 1, from an odd polynomial on each quarter of the turn: its error stays
// below 0.0002 (-74 dB), quiet enough under a synth's sine, and it costs a fraction of std::sin.
inline float sineOfPhase (float phase) noexcept
{
    phase -= std::floor (phase);

    // Fold the turn into [-1/4, 1/4], where sin is odd and monotonic.
    auto x = phase < 0.25f ? phase : (phase < 0.75f ? 0.5f - phase : phase - 1.0f);
    x *= 6.2831853f;
    const auto x2 = x * x;
    return x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f))));
}

// The correction for a unit step at phase 0 of a wave that moves dt per sample: subtracted from a falling step (a saw's
// wrap), added to a rising one.
inline float polyBlep (float t, float dt) noexcept
{
    if (t < dt)
    {
        t /= dt;
        return t + t - t * t - 1.0f;
    }

    if (t > 1.0f - dt)
    {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }

    return 0.0f;
}

enum class Waveform
{
    sine,
    triangle,
    saw,
    square // a pulse of a width from 5 % to 95 %; 50 % is a square
};

class Oscillator
{
public:
    // Up to just below half the sample rate.
    void setFrequency (double frequency, double sampleRate) noexcept
    {
        const auto step = frequency / sampleRate;
        increment = (float) (step < 0.0 ? 0.0 : (step > 0.49 ? 0.49 : step));
    }

    void setPhase (float newPhase) noexcept { phase = newPhase - std::floor (newPhase); }
    float getPhase() const noexcept { return phase; }

    float next (Waveform waveform, float pulseWidth = 0.5f) noexcept
    {
        const auto t = phase, dt = increment;
        auto value = 0.0f;

        switch (waveform)
        {
            case Waveform::sine:
                value = sineOfPhase (t);
                break;

            case Waveform::triangle:
                // Its harmonics fall at 12 dB per octave, so it aliases little without a correction.
                value = 4.0f * std::abs (t - 0.5f) - 1.0f;
                break;

            case Waveform::saw:
                value = 2.0f * t - 1.0f - polyBlep (t, dt);
                break;

            case Waveform::square:
            {
                const auto width = pulseWidth < 0.05f ? 0.05f : (pulseWidth > 0.95f ? 0.95f : pulseWidth);
                auto fall = t + 1.0f - width;
                fall -= std::floor (fall);
                value = (t < width ? 1.0f : -1.0f) + polyBlep (t, dt) - polyBlep (fall, dt);
                break;
            }
        }

        phase += dt;

        if (phase >= 1.0f)
            phase -= 1.0f;

        return value;
    }

private:
    float phase = 0.0f, increment = 0.0f;
};

//==============================================================================
enum class LfoShape
{
    sine,
    triangle,
    sawUp,
    sawDown,
    square,
    random // a new random value each cycle, held (sample and hold)
};

// A low-frequency oscillator, from -1 to 1. Its random values come from a seed, so a render repeats exactly.
class Lfo
{
public:
    void prepare (double newSampleRate) noexcept { sampleRate = newSampleRate; }
    void setRate (double hertz) noexcept { increment = (float) (hertz / sampleRate); }

    void reset (float startPhase = 0.0f, std::uint32_t seed = 1) noexcept
    {
        phase = startPhase - std::floor (startPhase);
        random.setSeed (seed);
        held = random.nextBipolar();
    }

    float next (LfoShape shape) noexcept
    {
        auto value = 0.0f;

        switch (shape)
        {
            case LfoShape::sine:
                value = sineOfPhase (phase);
                break;
            case LfoShape::triangle:
                value = 1.0f - 4.0f * std::abs (phase - 0.5f);
                break;
            case LfoShape::sawUp:
                value = 2.0f * phase - 1.0f;
                break;
            case LfoShape::sawDown:
                value = 1.0f - 2.0f * phase;
                break;
            case LfoShape::square:
                value = phase < 0.5f ? 1.0f : -1.0f;
                break;
            case LfoShape::random:
                value = held;
                break;
        }

        phase += increment;

        if (phase >= 1.0f)
        {
            phase -= std::floor (phase);
            held = random.nextBipolar();
        }

        return value;
    }

private:
    double sampleRate = 44100.0;
    float phase = 0.0f, increment = 0.0f, held = 0.0f;
    Random random;
};
} // namespace phazite::devices::dsp
