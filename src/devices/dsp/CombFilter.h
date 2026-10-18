// SPDX-License-Identifier: AGPL-3.0-or-later
// A comb filter (roadmap step 2.14): "Comb +" and "Comb -" among the filter types of the synth, the wavetable synth and
// the Filter device, as Serum, Vital, Surge XT, Bitwig's Filter+, Massive X and Ableton's Auto Filter each have one
// (docs/design.md, section 9). A delay of one period of its frequency, fed back into itself and added to what goes in.
// Comb + lets through the frequency and its multiples (the harmonics of a note at that pitch) and cuts what lies halfway
// between them. Comb -, fed back with its sign turned and half as long, lets through the frequency's odd multiples only:
// it sounds at the same pitch, hollow, as a square wave does (as Vital's negative flange does; Surge XT's negative comb
// sounds an octave lower instead). Resonance feeds more back: from none, a gentle comb of notches, as a flanger standing
// still makes, to peaks so narrow that it rings at its pitch, for about 0.8 s at 440 Hz (twice as long an octave down).
// As they narrow, the peaks rise by up to 8.5 dB while what lies between them falls as much, so that a sound neither
// vanishes nor jumps out.
#pragma once

#include <algorithm>
#include <vector>

namespace phazite::devices::dsp
{
struct CombCoefficients
{
    float delay = 2.0f;    // in samples: a period of the frequency, or half of one for Comb -
    float feedback = 0.0f; // the gain at which what comes out of the delay goes back in (below 0 for Comb -)
    float input = 1.0f;    // the gain at which what comes in goes into the delay, keeping the level inside the loop
    float output = 0.5f;   // the gain of what goes into the delay plus (minus, for Comb -) what comes out of it
    float sign = 1.0f;     // 1 for Comb +, -1 for Comb -
};

// The coefficients of a comb at a frequency (kept to at most a quarter of the sample rate for Comb -, half for Comb +),
// with resonance from 0 (notches only) to 1 (ringing).
CombCoefficients makeComb (bool negative, double frequency, double sampleRate, float resonance);

// The comb's gain (not in decibels) at a frequency, for an editor's curve and tests.
double getMagnitude (const CombCoefficients&, double frequency, double sampleRate);

// How long the comb rings after its input stops, in seconds, until it has fallen by 60 dB.
double getRingSeconds (const CombCoefficients&, double sampleRate);

// The longest delay, in samples, of a comb as low as a frequency.
int getLongestCombDelay (double lowestFrequency, double sampleRate);

//==============================================================================
// One channel of a comb filter. Its delay glides to a new frequency over a few samples, so that a sweep doesn't click.
class CombFilter
{
public:
    // Room for delays up to maximumDelay samples. Allocates: call it before playback.
    void prepare (int maximumDelay);

    // Silence: what it holds goes, and it takes the next coefficients at once. Real-time safe.
    void reset() noexcept;

    // Moves to new coefficients over the next `samples` samples, or at once after reset(). Real-time safe.
    void setTarget (const CombCoefficients&, int samples) noexcept;

    float process (float x) noexcept
    {
        if (stepsLeft > 0)
        {
            if (--stepsLeft == 0)
            {
                now = target;
            }
            else
            {
                now.delay += step.delay;
                now.feedback += step.feedback;
                now.input += step.input;
                now.output += step.output;
            }
        }

        // What went in a delay ago (one sample less, from the last one written), read between samples by 4-point
        // Lagrange interpolation: between the middle two of its points, its gain stays at or below 1 at every
        // frequency, so the loop stays stable however its delay moves.
        const auto back = std::clamp (now.delay, 2.0f, longest) - 1.0f;
        const auto whole = (int) back;
        const auto f = back - (float) whole;
        const auto a = at (whole - 1), b = at (whole), c = at (whole + 1), d = at (whole + 2);
        const auto fp1 = f + 1.0f, fm1 = f - 1.0f, fm2 = f - 2.0f;
        const auto delayed = (d * fp1 * f * fm1 - a * f * fm1 * fm2) * (1.0f / 6.0f) + (b * fp1 * fm1 * fm2 - c * fp1 * f * fm2) * 0.5f;

        // The guard is never reached by sound: it is there because, in principle, a loop whose delay moves can feed itself.
        const auto written = std::clamp (now.input * x + now.feedback * delayed, -guard, guard);
        writeIndex = (writeIndex + 1) & mask;
        buffer[(size_t) writeIndex] = written;
        used = true;
        return now.output * (written + now.sign * delayed);
    }

private:
    static constexpr float guard = 16.0f;

    std::vector<float> buffer { 0.0f, 0.0f, 0.0f, 0.0f };
    int mask = 3, writeIndex = 0;
    float longest = 2.0f; // the longest delay it has room for
    CombCoefficients now, target, step;
    int stepsLeft = 0;
    bool fresh = true, used = false;

    // A sample written before: 0 is the last one, 1 the one before it.
    float at (int delay) const noexcept { return buffer[(size_t) ((writeIndex - delay) & mask)]; }
};
} // namespace phazite::devices::dsp
