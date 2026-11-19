// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MultimodeFilter.h"

#include <algorithm>
#include <cmath>

namespace phazite::devices::dsp
{
namespace
{
    // A sung vowel's first three formants: their frequencies, levels and bandwidths (a tenor's, from the Csound manual's
    // table of formant values). In the order their second formants rise, from the darkest vowel to the brightest.
    struct Vowel
    {
        std::array<double, 3> frequency, decibels, bandwidth;
    };

    constexpr Vowel vowels[] = {
        { { 350.0, 600.0, 2700.0 }, { 0.0, -20.0, -17.0 }, { 40.0, 60.0, 100.0 } },  // U
        { { 400.0, 800.0, 2600.0 }, { 0.0, -10.0, -12.0 }, { 70.0, 80.0, 100.0 } },  // O
        { { 650.0, 1080.0, 2650.0 }, { 0.0, -6.0, -7.0 }, { 80.0, 90.0, 120.0 } },   // A
        { { 400.0, 1700.0, 2600.0 }, { 0.0, -14.0, -12.0 }, { 70.0, 80.0, 100.0 } }, // E
        { { 290.0, 1870.0, 2800.0 }, { 0.0, -15.0, -18.0 }, { 40.0, 90.0, 100.0 } }, // I
    };

    constexpr int numVowels = (int) std::size (vowels);

    // The gain after the vowel's formants at half resonance (10 dB): they pass a small part of a sound, so a saw comes out
    // within a few decibels of its level on average; less as they widen and more as they narrow, keeping that.
    constexpr double vowelOutput = 3.16;

    // At the most resonance: the phaser's feedback, and the held sound's (12 dB more at the cutoff than below it).
    constexpr double mostPhaserFeedback = 0.85, mostHoldFeedback = 0.6;

    double interpolate (double from, double to, double t)
    {
        return from + t * (to - from);
    }

    // The formants for a cutoff: the vowels' second formants mark where each vowel is, and between two of them the
    // formants glide in octaves; beyond U or I, that vowel's formants move with the cutoff.
    void makeVowel (MultimodeCoefficients& c, double cutoff, double amount, double sampleRate)
    {
        auto first = 0;
        auto along = 0.0, shift = 1.0;

        if (cutoff <= vowels[0].frequency[1])
        {
            shift = cutoff / vowels[0].frequency[1];
        }
        else if (cutoff >= vowels[numVowels - 1].frequency[1])
        {
            first = numVowels - 2;
            along = 1.0;
            shift = cutoff / vowels[numVowels - 1].frequency[1];
        }
        else
        {
            while (cutoff > vowels[first + 1].frequency[1])
                ++first;

            along = std::log (cutoff / vowels[first].frequency[1]) / std::log (vowels[first + 1].frequency[1] / vowels[first].frequency[1]);
        }

        const auto& from = vowels[first];
        const auto& to = vowels[first + 1];

        // Resonance narrows them: twice as wide as the table's with none, half as wide at the most.
        const auto narrowing = std::pow (2.0, 1.0 - 2.0 * amount);

        for (size_t f = 0; f < 3; ++f)
        {
            const auto frequency = shift * std::exp (interpolate (std::log (from.frequency[f]), std::log (to.frequency[f]), along));
            const auto bandwidth =
                shift * narrowing * std::exp (interpolate (std::log (from.bandwidth[f]), std::log (to.bandwidth[f]), along));
            c.sections[f] = makeFilter (FilterShape::bandPass, frequency, sampleRate, frequency / bandwidth);
            c.gains[f] = (float) std::pow (10.0, interpolate (from.decibels[f], to.decibels[f], along) / 20.0);
        }

        c.numSections = 3;
        c.output = (float) (vowelOutput / std::sqrt (narrowing));
    }
} // namespace

MultimodeCoefficients makeMultimode (const FilterSettings& settings, double sampleRate)
{
    MultimodeCoefficients c;
    c.mode = settings.mode;
    const auto amount = std::clamp ((double) settings.resonance, 0.0, 1.0);
    const auto frequency = settings.frequency;

    switch (settings.mode)
    {
        case FilterMode::lowPass:
        case FilterMode::highPass:
        case FilterMode::bandPass:
        case FilterMode::notch:
        {
            const auto shape = settings.mode == FilterMode::lowPass    ? FilterShape::lowPass
                               : settings.mode == FilterMode::highPass ? FilterShape::highPass
                               : settings.mode == FilterMode::bandPass ? FilterShape::bandPass
                                                                       : FilterShape::notch;

            if (! settings.steep)
            {
                c.sections[0] = makeFilter (shape, frequency, sampleRate, settings.q);
                c.numSections = 1;
                break;
            }

            // 24 dB: low-pass and high-pass as a fourth-order Butterworth's two sections, the second carrying the resonance
            // (-3 dB at the cutoff without it); band-pass and notch as two alike, narrower and deeper.
            const auto plain = settings.mode == FilterMode::bandPass || settings.mode == FilterMode::notch;
            c.sections[0] = makeFilter (shape, frequency, sampleRate, plain ? settings.q : getButterworthQ (4, 0));
            c.sections[1] =
                makeFilter (shape, frequency, sampleRate, plain ? settings.q : getButterworthQ (4, 1) * std::pow (2.0, amount * 4.0));
            c.numSections = 2;
            break;
        }

        case FilterMode::combPlus:
        case FilterMode::combMinus:
            c.comb = makeComb (settings.mode == FilterMode::combMinus, frequency, sampleRate, settings.resonance);
            c.numSections = 0;
            break;

        case FilterMode::vowel:
            makeVowel (c, frequency, amount, sampleRate);
            break;

        case FilterMode::phaser:
        {
            // Six first-order all-pass stages, as three two-pole sections with both poles alike (a Q of a half): their
            // phase turns through three half turns against what goes in, at about a quarter of the cutoff, the cutoff
            // and nearly four times it, the notches. Feedback raises the peaks between them; the output gain keeps those
            // within about 3.5 dB.
            for (auto& section : c.sections)
                section = makeFilter (FilterShape::allPass, frequency, sampleRate, 0.5);

            c.numSections = 3;
            c.feedback = (float) (mostPhaserFeedback * amount);
            c.output = (float) (0.5 * std::sqrt (1.0 - (double) c.feedback));
            break;
        }

        case FilterMode::sampleAndHold:
            // Twice the cutoff's rate keeps what is below the cutoff and folds back what is above it. What it holds goes
            // back in with its sign turned, ringing at the cutoff; the output gain keeps what is steady at its level.
            c.holdStep = (float) std::clamp (2.0 * frequency / sampleRate, 1.0e-4, 1.0);
            c.feedback = (float) (mostHoldFeedback * amount);
            c.output = 1.0f + c.feedback;
            c.numSections = 0;
            break;
    }

    return c;
}

double getMagnitude (const MultimodeCoefficients& c, double frequency, double sampleRate)
{
    return std::abs (getResponse (c, frequency, sampleRate));
}

bool hasResponse (FilterMode mode) noexcept
{
    return mode != FilterMode::sampleAndHold;
}

std::complex<double> getResponse (const MultimodeCoefficients& c, double frequency, double sampleRate)
{
    switch (c.mode)
    {
        case FilterMode::lowPass:
        case FilterMode::highPass:
        case FilterMode::bandPass:
        case FilterMode::notch:
        {
            std::complex<double> response (1.0, 0.0);

            for (int s = 0; s < c.numSections; ++s)
                response *= getResponse (c.sections[(size_t) s], frequency, sampleRate);

            return response;
        }

        case FilterMode::combPlus:
        case FilterMode::combMinus:
            return getResponse (c.comb, frequency, sampleRate);

        case FilterMode::vowel:
        {
            std::complex<double> sum;

            for (size_t s = 0; s < c.sections.size(); ++s)
                sum += (double) c.gains[s] * getResponse (c.sections[s], frequency, sampleRate);

            return (double) c.output * sum;
        }

        case FilterMode::phaser:
        {
            // y = output (x + a), with a = A (x + feedback a): the loop solved at once.
            std::complex<double> stages (1.0, 0.0);

            for (const auto& section : c.sections)
                stages *= getResponse (section, frequency, sampleRate);

            return (double) c.output * (1.0 + stages / (1.0 - (double) c.feedback * stages));
        }

        case FilterMode::sampleAndHold:
            return 1.0;
    }

    return 1.0;
}

//==============================================================================
namespace
{
    // Whether the loop is a 24 dB low-pass's or high-pass's second section, whose Q starts at a fourth-order Butterworth's.
    bool isButterworthPair (FilterMode mode, bool steep) noexcept
    {
        return steep && (mode == FilterMode::lowPass || mode == FilterMode::highPass);
    }

    // Clean's damping for the loop's section at a Resonance.
    double getCleanDamping (FilterMode mode, bool steep, double resonance, ResonanceCurve curve) noexcept
    {
        return isButterworthPair (mode, steep) ? 1.0 / (getButterworthQ (4, 1) * std::pow (2.0, 4.0 * resonance))
                                               : 1.0 / curve ((float) resonance);
    }

    // How much of a sine's amplitude softClip passes as its fundamental, at singingLevel: 1 - 2/9 s^2 + 5/81 s^4 from the
    // curve's series (x - 8/27 x^3 + 8/81 x^5), good to a hundred-thousandth there. The loop sings where the damping's
    // fundamental is nothing: kLarge = bend times that, so kSmall = kLarge (1 - 1 / that).
    constexpr double singingShare = 1.0 - 2.0 / 9.0 * (double) singingLevel * singingLevel
                                    + 5.0 / 81.0 * (double) singingLevel * singingLevel * singingLevel * singingLevel;
    constexpr double drivenFromResonance = 0.9; // where the Driven damping leaves Clean's
} // namespace

float getDrivenLargeDamping (FilterMode mode, bool steep, ResonanceCurve curve) noexcept
{
    return (float) getCleanDamping (mode, steep, 0.0, curve);
}

float getDrivenDamping (FilterMode mode, bool steep, float resonance, ResonanceCurve curve) noexcept
{
    const auto amount = std::clamp ((double) resonance, 0.0, 1.0);

    if (amount <= drivenFromResonance)
        return (float) getCleanDamping (mode, steep, amount, curve);

    const auto from = getCleanDamping (mode, steep, drivenFromResonance, curve);
    const auto singing = (double) getDrivenLargeDamping (mode, steep, curve) * (1.0 - 1.0 / singingShare);
    return (float) (from + (amount - drivenFromResonance) / (1.0 - drivenFromResonance) * (singing - from));
}

DrivenCoefficients makeDriven (const FilterSettings& settings, double sampleRate, ResonanceCurve curve)
{
    DrivenCoefficients c;
    const auto frequency = std::clamp (settings.frequency, 1.0, 0.49 * sampleRate); // tan() grows without bound at Nyquist
    const auto shape = settings.mode == FilterMode::highPass   ? FilterShape::highPass
                       : settings.mode == FilterMode::bandPass ? FilterShape::bandPass
                       : settings.mode == FilterMode::notch    ? FilterShape::notch
                                                               : FilterShape::lowPass;

    // 24 dB: a linear section in front, as Clean's first: a fourth-order Butterworth's for a low-pass or high-pass, one
    // alike for a band-pass or notch.
    if (settings.steep)
    {
        c.hasFront = true;
        c.front = makeFilter (shape, frequency, sampleRate,
                              isButterworthPair (settings.mode, true) ? getButterworthQ (4, 0) : curve (settings.resonance));
    }

    const auto g = std::tan (juce::MathConstants<double>::pi * frequency / sampleRate);
    c.g = (float) g;
    c.a = (float) (1.0 + g * g);
    c.kLarge = getDrivenLargeDamping (settings.mode, settings.steep, curve);
    c.kSmall = getDrivenDamping (settings.mode, settings.steep, settings.resonance, curve);
    c.bend = c.kLarge - c.kSmall;
    c.bandGain = (float) getCleanDamping (settings.mode, settings.steep, std::clamp ((double) settings.resonance, 0.0, 1.0), curve);

    switch (settings.mode)
    {
        case FilterMode::highPass:
            c.mx = 1.0f;
            c.mlp = -1.0f;
            c.md = -1.0f;
            break;

        case FilterMode::bandPass:
            c.mv = c.bandGain - c.kSmall;
            c.mlp = 0.0f;
            c.md = 1.0f;
            break;

        case FilterMode::notch:
            c.mx = 1.0f;
            c.mlp = 0.0f;
            c.md = -1.0f;
            break;

        case FilterMode::lowPass:
        case FilterMode::combPlus:
        case FilterMode::combMinus:
        case FilterMode::vowel:
        case FilterMode::phaser:
        case FilterMode::sampleAndHold:
            break;
    }

    return c;
}

double getMagnitude (const DrivenCoefficients& c, double frequency, double sampleRate)
{
    return std::abs (getResponse (c, frequency, sampleRate));
}

std::complex<double> getResponse (const DrivenCoefficients& c, double frequency, double sampleRate)
{
    // The loop for a quiet sound: a state-variable section with the damping kSmall (a little above 0 where it sings, so that
    // its peak goes off any scale rather than to nothing).
    frequency = std::clamp (frequency, 0.0, 0.4999 * sampleRate);
    const std::complex<double> s (0.0, std::tan (juce::MathConstants<double>::pi * frequency / sampleRate) / (double) c.g);
    const auto k = std::max ((double) c.kSmall, 1.0e-4);
    const auto denominator = s * s + k * s + 1.0;
    const auto loop = (double) c.mx + ((double) c.mv * s + (double) c.mlp + (double) c.md * k * s) / denominator;
    return loop * (c.hasFront ? getResponse (c.front, frequency, sampleRate) : std::complex<double> (1.0, 0.0));
}

//==============================================================================
void MultimodeFilter::prepare (int longestCombDelay)
{
    comb.prepare (longestCombDelay);
    reset();
}

void MultimodeFilter::reset() noexcept
{
    for (auto& section : sections)
        section.reset();

    comb.reset();
    holdPhase = 1.0f;
    held = 0.0f;
}

void MultimodeFilter::set (const MultimodeCoefficients& next, int samples) noexcept
{
    // The kind of filter: one that holds sound of its own (sections, a comb, formants, stages, a held sample).
    const auto kind = [] (FilterMode mode)
    {
        switch (mode)
        {
            case FilterMode::lowPass:
            case FilterMode::highPass:
            case FilterMode::bandPass:
            case FilterMode::notch:
                return 0;
            case FilterMode::combPlus:
            case FilterMode::combMinus:
                return 1;
            case FilterMode::vowel:
                return 2;
            case FilterMode::phaser:
                return 3;
            case FilterMode::sampleAndHold:
                return 4;
        }

        return 0;
    };

    if (kind (next.mode) != kind (now.mode))
        reset();

    now = next;

    if (kind (next.mode) == 1)
        comb.setTarget (next.comb, samples);
}
} // namespace phazite::devices::dsp
