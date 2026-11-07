// SPDX-License-Identifier: AGPL-3.0-or-later
// Oscillators for Phazite's devices: a band-limited one for the synth's voices, the wavetable synth's sub and the
// vocoder's buzz, whose saw, pulse and triangle are read from band-limited tables as the wavetable synth's are
// (wavetable/Wavetable.h), so that high notes don't alias into harsh tones; and a low-frequency one for modulation.
// Real-time safe, once made.
#pragma once

#include "Random.h"
#include "devices/wavetable/Wavetable.h"

#include <algorithm>
#include <cmath>
#include <memory>

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

// The correction for a unit step at phase 0 of a wave that moves dt per sample: subtracted from a falling step, added to
// a rising one (the wavetable synth's hard sync smooths its jump back with it).
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

// The band-limited waves the oscillator reads: a saw rising from -1 to 1 and a triangle falling from 1 to -1 and back, each
// with every harmonic of the plain wave at its level, kept at the levels a wavetable keeps (a copy each half octave with
// fewer harmonics), each finely enough to be read by straight lines between its samples (Wavetable::forOscillator()). Made
// the first time they are asked for, which takes a few milliseconds, and kept: an oscillator asks as it is made, which is
// never on the audio thread.
struct BandLimitedWaves
{
    std::shared_ptr<const Wavetable> saw, triangle;
};

const BandLimitedWaves& getBandLimitedWaves();

class Oscillator
{
public:
    Oscillator();

    // Up to just below half the sample rate. Each wave plays every harmonic whose alias stays above 20 kHz.
    void setFrequency (double frequency, double sampleRate) noexcept
    {
        const auto step = frequency / sampleRate;
        increment = (float) (step < 0.0 ? 0.0 : (step > 0.49 ? 0.49 : step));

        const auto limit = std::clamp (1.0 - 20000.0 / sampleRate, 0.45, 0.9);
        const auto level = Wavetable::chooseLevel (limit / std::max (1.0e-9, (double) increment));
        sawLevel = saw->getLevel (0, level);
        triangleLevel = triangle->getLevel (0, level);
    }

    void setPhase (float newPhase) noexcept { phase = newPhase - std::floor (newPhase); }
    float getPhase() const noexcept { return phase; }

    float next (Waveform waveform, float pulseWidth = 0.5f) noexcept
    {
        const auto t = phase;
        auto value = 0.0f;

        switch (waveform)
        {
            case Waveform::sine:
                value = sineOfPhase (t);
                break;

            case Waveform::triangle:
                value = read (triangleLevel, t);
                break;

            case Waveform::saw:
                value = read (sawLevel, t);
                break;

            case Waveform::square:
            {
                // Two saws a width apart, one taken from the other, so that the width can move: high for the width, then low.
                const auto width = pulseWidth < 0.05f ? 0.05f : (pulseWidth > 0.95f ? 0.95f : pulseWidth);
                const auto behind = t < width ? t - width + 1.0f : t - width;
                value = read (sawLevel, behind) - read (sawLevel, t) + 2.0f * width - 1.0f;
                break;
            }
        }

        phase += increment;

        if (phase >= 1.0f)
            phase -= 1.0f;

        return value;
    }

private:
    const Wavetable* saw = nullptr;
    const Wavetable* triangle = nullptr;
    Wavetable::Level sawLevel, triangleLevel;
    float phase = 0.0f, increment = 0.0f;

    // A level's wave at a place in its cycle, from 0 to 1.
    static float read (const Wavetable::Level& level, float place) noexcept
    {
        const auto length = (float) level.length;
        auto at = place * length;

        if (at >= length) // a place just under 1, rounded up
            at -= length;

        const auto index = (int) at;
        const auto* s = level.samples + index;
        return s[0] + (at - (float) index) * (s[1] - s[0]);
    }
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

    // A shape's value at a point in its cycle (0 to 1), from -1 to 1: for the LFO, and its editor's picture of it. The random
    // shape's values are its own (held a cycle each): 0 here.
    static float getValue (LfoShape shape, float phase) noexcept
    {
        switch (shape)
        {
            case LfoShape::sine:
                return sineOfPhase (phase);
            case LfoShape::triangle:
                return 1.0f - 4.0f * std::abs (phase - 0.5f);
            case LfoShape::sawUp:
                return 2.0f * phase - 1.0f;
            case LfoShape::sawDown:
                return 1.0f - 2.0f * phase;
            case LfoShape::square:
                return phase < 0.5f ? 1.0f : -1.0f;
            case LfoShape::random:
                break;
        }

        return 0.0f;
    }

    float next (LfoShape shape) noexcept
    {
        const auto value = shape == LfoShape::random ? held : getValue (shape, phase);
        phase += increment;

        if (phase >= 1.0f)
        {
            phase -= std::floor (phase);
            held = random.nextBipolar();
        }

        return value;
    }

    float getPhase() const noexcept { return phase; } // from 0 to 1, where it is in its cycle

private:
    double sampleRate = 44100.0;
    float phase = 0.0f, increment = 0.0f, held = 0.0f;
    Random random;
};
} // namespace phazite::devices::dsp
