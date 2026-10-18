// SPDX-License-Identifier: AGPL-3.0-or-later
// The filter types of Phazite's filters, the synth's, the wavetable synth's and the Filter device's (roadmap steps 2.10,
// 2.14 and 2.15), one channel of them, so that every device offers the same types and each sounds the same in all of
// them (docs/design.md, section 9):
// - low-pass, high-pass, band-pass and notch at 12 or 24 dB per octave (Filters.h);
// - Comb + and Comb - (CombFilter.h);
// - Vowel: three band-passes at the formants of a sung vowel, which the cutoff picks, as the second formant's frequency,
//   from U through O, A and E to I, moving all three beyond them;
// - Phaser: six all-pass stages fed back, mixed with what goes in, whose three notches the cutoff moves;
// - Sample & Hold: the sound held at twice the cutoff's rate, aliasing on purpose, as a lo-fi sampler does.
// The resonance sharpens each: it narrows the formants, feeds the phaser back, and feeds the held sound back to ring at
// the cutoff. Each device keeps its own curve from resonance to a 12 dB section's Q, so projects sound as they did.
#pragma once

#include "CombFilter.h"
#include "Filters.h"

#include <array>

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

struct FilterSettings
{
    FilterMode mode = FilterMode::lowPass;
    double frequency = 20000.0; // the cutoff, in hertz
    float resonance = 0.0f;     // from 0 to 1
    bool steep = false;         // 24 dB per octave rather than 12, for low-pass, high-pass, band-pass and notch
    double q = 0.70710678;      // a 12 dB section's Q, made from the resonance by the device's own curve
};

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

private:
    MultimodeCoefficients now;
    std::array<Filter, 3> sections;
    CombFilter comb;
    float holdPhase = 1.0f, held = 0.0f; // the sample and hold's: a new sample at once after a reset
};
} // namespace phazite::devices::dsp
