// SPDX-License-Identifier: AGPL-3.0-or-later
// The filter types of Phazite's filters, the synth's, the wavetable synth's and the Filter device's (roadmap steps 2.10,
// 2.14 and 2.15), one channel of them, so that every device offers the same types, in the same order and by the same
// names, and each sounds the same in all of them (docs/design.md, section 9):
// - low-pass, high-pass, band-pass and notch at 12 or 24 dB per octave, as each device's Slope says (Filters.h);
// - Comb + and Comb - (CombFilter.h);
// - Vowel: three band-passes at the formants of a sung vowel, which the cutoff picks, as the second formant's frequency,
//   from U through O, A and E to I, moving all three beyond them;
// - Phaser: six all-pass stages fed back, mixed with what goes in, whose three notches the cutoff moves;
// - Sample & Hold: the sound held at twice the cutoff's rate, aliasing on purpose, as a lo-fi sampler does.
// The resonance sharpens each: it narrows the formants, feeds the phaser back, and feeds the held sound back to ring at
// the cutoff. A 12 dB section's Q comes from the resonance by getResonanceQ(), but in the synth, whose curve is its own.
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
