// SPDX-License-Identifier: AGPL-3.0-or-later
// The filter types of Phazite's filters, the synths', the sampler's and the Filter device's (roadmap steps 2.10,
// 2.14 and 2.15), one channel of them, so that every device offers the same types, in the same order and by the same
// names, and each sounds the same in all of them (docs/design.md, section 9):
// - low-pass, high-pass, band-pass and notch at 12 or 24 dB per octave, as each device's Slope says (Filters.h);
// - Comb + and Comb - (CombFilter.h);
// - Vowel: three band-passes at the formants of a sung vowel, which the cutoff picks, as the second formant's frequency,
//   from U through O, A and E to I, moving all three beyond them;
// - Phaser: six all-pass stages fed back, mixed with what goes in, whose three notches the cutoff moves;
// - Sample & Hold: the sound held at twice the cutoff's rate, aliasing on purpose, as a lo-fi sampler does.
// The resonance sharpens each: it narrows the formants, feeds the phaser back, and feeds the held sound back to ring at
// the cutoff. A 12 dB section's Q comes from the resonance by getResonanceQ(), but in the Synth, whose curve is its own
// (getSynthResonanceQ()).
#pragma once

#include "CombFilter.h"
#include "Filters.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <string_view>

namespace phazite::devices::dsp
{
enum class FilterMode
{
    lowPass,
    highPass,
    bandPass,
    notch,
    combPlus,
    combMinus,
    vowel,
    phaser,
    sampleAndHold
};

// What each device's Type offers, in the order of FilterMode, a type's value being its mode's number; and its Slope.
inline constexpr std::string_view filterModeNames[] = { "Low Pass", "High Pass", "Band Pass", "Notch",        "Comb +",
                                                        "Comb -",   "Vowel",     "Phaser",    "Sample & Hold" };
inline constexpr std::string_view filterSlopeNames[] = { "12 dB", "24 dB" };

static_assert (std::size (filterModeNames) == (size_t) FilterMode::sampleAndHold + 1);

// A Type's value as its mode, and a Slope's as whether it is 24 dB per octave.
inline FilterMode toFilterMode (float value) noexcept
{
    return (FilterMode) std::clamp ((int) std::lround (value), 0, (int) std::size (filterModeNames) - 1);
}

inline bool isSteep (float slope) noexcept
{
    return slope > 0.5f;
}

// A 12 dB section's Q for a resonance from 0 to 1: from a Butterworth's (0.71, flat) to 16, ringing.
inline double getResonanceQ (float resonance) noexcept
{
    return 0.70710678 * std::pow (2.0, 4.5 * (double) std::clamp (resonance, 0.0f, 1.0f));
}

// The Synth's own curve for a 12 dB section's Q: from 0.5, softer than a Butterworth, rounding a sound's top as an analog
// synth's filter does with its resonance down, to 25, near self-oscillation.
inline double getSynthResonanceQ (float resonance) noexcept
{
    return 0.5 * std::pow (50.0, (double) std::clamp (resonance, 0.0f, 1.0f));
}

// How far a softer note closes a synth's filter, in semitones, with its Filter Velocity at full: four octaves at velocity 0,
// nothing at full velocity, the same in every synth that has one (the cutoff moves by this, times the Filter Velocity,
// times how far the velocity is under 1).
inline constexpr float filterVelocityRange = 48.0f;

struct FilterSettings
{
    FilterMode mode = FilterMode::lowPass;
    double frequency = 20000.0; // the cutoff, in hertz
    float resonance = 0.0f;     // from 0 to 1
    bool steep = false;         // 24 dB per octave rather than 12, for low-pass, high-pass, band-pass and notch
    double q = 0.70710678;      // a 12 dB section's Q, made from the resonance (getResonanceQ())
};

inline bool areSame (const FilterSettings& a, const FilterSettings& b) noexcept
{
    return a.mode == b.mode && juce::exactlyEqual (a.frequency, b.frequency) && juce::exactlyEqual (a.resonance, b.resonance)
           && a.steep == b.steep && juce::exactlyEqual (a.q, b.q);
}

// What a channel plays with, made once a step for all of them.
struct MultimodeCoefficients
{
    FilterMode mode = FilterMode::lowPass;
    std::array<FilterCoefficients, 3> sections {}; // in a row, or side by side for the vowel's formants
    int numSections = 1;
    std::array<float, 3> gains { 1.0f, 1.0f, 1.0f }; // the vowel's formants'
    float feedback = 0.0f;                           // the phaser's, and the held sound's
    float output = 1.0f;                             // a gain on what comes out
    float holdStep = 1.0f;                           // of a hold, each sample
    CombCoefficients comb {};
};

// The coefficients for some settings at a sample rate. Real-time safe.
MultimodeCoefficients makeMultimode (const FilterSettings&, double sampleRate);

// The gain (not in decibels) at a frequency, for an editor's curve and tests. Sample & Hold isn't a linear filter: it
// reports 1.
double getMagnitude (const MultimodeCoefficients&, double frequency, double sampleRate);

//==============================================================================
// The Driven character of low-pass, high-pass, band-pass and notch (roadmap step S6.6), which the Filter device plays (and
// the synths may take later): the same state-variable section, integrated with the trapezoidal rule, with a curve inside its
// loop, as Vadim Zavalishin's "The Art of VA Filter Design" builds the nonlinear state-variable filter: the band-pass state's
// feedback, the damping, is a Butterworth's (kLarge) less the resonance's share (bend), and that share passes through
// softClip (Math.h), so that it can't grow past the curve's ceiling. A quiet sound meets the damping kSmall = kLarge - bend,
// the Q the Resonance gives; a loud one meets more of kLarge, its resonance saturating. Where kSmall falls below 0 (the top
// of the Resonance), the filter sings at its cutoff with nothing coming in, at the level where the curve's bend balances
// the loss: singingLevel. The loop is solved within each sample: a guess from the last sample's slope of the curve, then
// drivenNewtonSteps Newton steps (the loop's equation rises steadily with the band-pass state, so they converge from any
// guess), never a sample late. At 24 dB a linear section goes before the loop, as the Clean 24 dB's first.
inline constexpr float singingLevel = 0.25f; // the band-pass and low-pass states' peak, self-oscillating: -12 dBFS
inline constexpr int drivenNewtonSteps = 2;
inline constexpr float singingKick = 1.0e-4f; // what starts a loop set to sing from rest

struct DrivenCoefficients
{
    FilterCoefficients front; // a linear section before the loop (24 dB), when hasFront
    bool hasFront = false;
    float g = 0.0f;              // the loop's integrators' gain, tan (pi f / rate)
    float a = 1.0f;              // 1 + g^2
    float kLarge = 1.4142135f;   // its damping for a loud sound
    float bend = 0.0f;           // the resonance's share of it, through the curve
    float kSmall = 1.4142135f;   // kLarge - bend: its damping for a quiet one, 1 / Q (below 0, it sings)
    float bandGain = 1.4142135f; // Clean's damping at the same Resonance: the band-pass's gain on its state, a peak of 1 while quiet
    // What comes out: in, the band-pass state, the low-pass state and the damping term (the curve's), so mixed:
    // low-pass (0, 0, 1, 0); high-pass, in less the damping and the low-pass (1, 0, -1, -1); band-pass, the damping and the
    // rest of Clean's gain (0, bandGain - kSmall, 0, 1), so that at its cutoff it follows what comes in at any level, where
    // the damping alone gives way as a loud sound saturates it; notch, in less the damping (1, 0, 0, -1).
    float mx = 0.0f, mv = 0.0f, mlp = 1.0f, md = 0.0f;
};

// The Driven coefficients for some settings (their mode one of the four; the Resonance alone sets the Q, by
// getDrivenDamping()) at the rate the filter plays at. Real-time safe.
DrivenCoefficients makeDriven (const FilterSettings&, double sampleRate);

// The loop's damping for a quiet sound at a Resonance from 0 to 1 (1 / Q): Clean's (getResonanceQ(), or a 24 dB low-pass's
// or high-pass's second section's) up to 90 %, the same shape, then on in a straight line to a little below 0 at 100 %,
// where it sings at singingLevel. And the damping for a loud one, a Butterworth's (the Resonance at 0).
float getDrivenDamping (FilterMode, bool steep, float resonance) noexcept;
float getDrivenLargeDamping (FilterMode, bool steep) noexcept;

// The gain (not in decibels) of the Driven filter for a quiet sound, the curve straight, at a frequency: its peak goes off
// any scale where it sings. For an editor's curve and tests.
double getMagnitude (const DrivenCoefficients&, double frequency, double sampleRate);

//==============================================================================
// One channel, with what it holds of the sound.
class MultimodeFilter
{
public:
    // Room for a comb's longest delay, in samples. Allocates: call it before playback.
    void prepare (int longestCombDelay);

    // Silence: what it holds goes. Real-time safe.
    void reset() noexcept;

    // New coefficients, a comb's delay gliding to them over the next `samples` samples. Another kind of filter than the
    // last starts from silence. Real-time safe.
    void set (const MultimodeCoefficients&, int samples) noexcept;

    float process (float x) noexcept
    {
        switch (now.mode)
        {
            case FilterMode::lowPass:
            case FilterMode::highPass:
            case FilterMode::bandPass:
            case FilterMode::notch:
                for (int s = 0; s < now.numSections; ++s)
                    x = sections[(size_t) s].process (x, now.sections[(size_t) s]);

                return x;

            case FilterMode::combPlus:
            case FilterMode::combMinus:
                return comb.process (x);

            case FilterMode::vowel:
            {
                auto y = 0.0f;

                for (size_t s = 0; s < sections.size(); ++s)
                    y += now.gains[s] * sections[s].process (x, now.sections[s]);

                return now.output * y;
            }

            case FilterMode::phaser:
            {
                // What comes out of the stages goes back in at once: they will put out gain * w + offset for what goes
                // in, w = x + feedback * that, so the loop is solved for this sample before they take it.
                auto gain = 1.0f, offset = 0.0f;

                for (size_t s = 0; s < sections.size(); ++s)
                {
                    const auto instant = sections[s].getInstant (now.sections[s]);
                    offset = instant.gain * offset + instant.offset;
                    gain *= instant.gain;
                }

                auto y = x + now.feedback * (gain * x + offset) / (1.0f - now.feedback * gain);

                for (size_t s = 0; s < sections.size(); ++s)
                    y = sections[s].process (y, now.sections[s]);

                return now.output * (x + y);
            }

            case FilterMode::sampleAndHold:
                // A new sample at each hold, the one before fed back with its sign turned, which rings at the cutoff.
                holdPhase += now.holdStep;

                if (holdPhase >= 1.0f)
                {
                    holdPhase -= 1.0f;
                    held = x - now.feedback * held;
                }

                return now.output * held;
        }

        return x;
    }

    // The Driven character (makeDriven()), with a state of its own: silence, new coefficients (at once), and a sample.
    // Real-time safe.
    void resetDriven() noexcept
    {
        front.reset();
        s1 = s2 = lastV = lastLow = 0.0f;
        guessAt = guessCurve = 0.0f;
        guessSlope = 1.0f;
    }

    void setDriven (const DrivenCoefficients& next) noexcept { driven = next; }

    float processDriven (float x) noexcept
    {
        const auto& c = driven;

        if (c.hasFront)
            x = front.process (x, c.front);

        // Set to sing, from rest: a kick 80 dB under full scale starts it, as an analog filter starts from its own noise, so that
        // it sings from silence too.
        if (c.kSmall < 0.0f && juce::exactlyEqual (s1, 0.0f) && juce::exactlyEqual (s2, 0.0f))
            x += singingKick;

        // The band-pass state v solves a v + g D(v) = b, D(v) = kLarge v - bend softClip (v), b what the states and x give:
        // A v - B softClip (v) = b. A guess from the line that touches the curve where the last sample's last step was taken,
        // then Newton's steps; each works out the curve and its slope with one division.
        const auto b = c.g * (x - s2) + s1;
        const auto A = c.a + c.g * c.kLarge, B = c.g * c.bend;
        auto v = (b + B * (guessCurve - guessSlope * guessAt)) / (A - B * guessSlope);

        for (int step = 0; step < drivenNewtonSteps; ++step)
        {
            guessAt = v;
            curveAndSlope (v, guessCurve, guessSlope);
            v -= (A * v - B * guessCurve - b) / (A - B * guessSlope);
        }

        const auto low = c.g * v + s2;
        s1 = 2.0f * v - s1;
        s2 = 2.0f * low - s2;
        lastV = v;
        lastLow = low;

        auto y = c.mx * x + c.mv * v + c.mlp * low;

        if (! juce::exactlyEqual (c.md, 0.0f))
        {
            auto curve = 0.0f, slope = 0.0f;
            curveAndSlope (v, curve, slope);
            y += c.md * (c.kLarge * v - c.bend * curve);
        }

        return y;
    }

    // Takes another Driven filter's loop as it is, playing at another rate (or the same): its states are the analog
    // filter's band-pass and low-pass and what flows into them, which mean the same at any rate. gRatio: this filter's g over
    // the other's. Its section in front, a gentle linear one, starts from where the other's is. Real-time safe.
    void takeDrivenState (const MultimodeFilter& other, float gRatio) noexcept
    {
        front = other.front;
        s1 = other.lastV + gRatio * (other.s1 - other.lastV);
        s2 = other.lastLow + gRatio * (other.s2 - other.lastLow);
        lastV = other.lastV;
        lastLow = other.lastLow;
        guessAt = other.guessAt;
        guessCurve = other.guessCurve;
        guessSlope = other.guessSlope;
    }

    const DrivenCoefficients& getDriven() const noexcept { return driven; }

    // The Driven loop's two states (its trapezoidal integrators'), for tests of its bounds.
    float getDrivenState1() const noexcept { return s1; }
    float getDrivenState2() const noexcept { return s2; }

private:
    MultimodeCoefficients now;
    std::array<Filter, 3> sections;
    CombFilter comb;
    float holdPhase = 1.0f, held = 0.0f; // the sample and hold's: a new sample at once after a reset

    // The Driven character's: its coefficients, its section in front, its loop's states (the trapezoidal integrators'), its
    // band-pass and low-pass at the last sample, and where the last Newton step was taken, with the curve and its slope there.
    DrivenCoefficients driven;
    Filter front;
    float s1 = 0.0f, s2 = 0.0f, lastV = 0.0f, lastLow = 0.0f;
    float guessAt = 0.0f, guessCurve = 0.0f, guessSlope = 1.0f;

    // softClip (Math.h) and its slope, ((9 - u^2) / (3 (3 + u^2)))^2, flat from 3 on.
    static void curveAndSlope (float u, float& curve, float& slope) noexcept
    {
        if (std::abs (u) >= 3.0f)
        {
            curve = u > 0.0f ? 1.0f : -1.0f;
            slope = 0.0f;
            return;
        }

        const auto u2 = u * u;
        const auto over = 1.0f / (27.0f + 9.0f * u2);
        curve = u * (27.0f + u2) * over;
        const auto root = (9.0f - u2) * over;
        slope = 9.0f * root * root;
    }
};
} // namespace phazite::devices::dsp
