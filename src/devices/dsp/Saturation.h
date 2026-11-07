// SPDX-License-Identifier: AGPL-3.0-or-later
// Curves that saturate the sound, with their antiderivatives, and first-order antiderivative anti-aliasing (ADAA: Parker,
// Zavalishin and Le Bivic, DAFx-16; Bilbao, Esqueda, Parker and Valimaki, 2017) to play them with: each output the curve's
// average between the last two inputs, worked out from its antiderivative, so that the harmonics a curve adds fall away
// steeply rather than fold back as aliasing. Played at twice the sound's rate (Oversampling.h or juce::dsp::Oversampling),
// whose half-band filters then take away what is left above the sound's band. Averaging dulls the top octave (2.4 dB at
// 20 kHz from 44.1 kHz at twice the rate), so a short linear-phase filter after it makes that up: what a curve leaves
// straight comes out level within 0.3 dB to 20 kHz, 4 samples (at its rate) late. Real-time safe.
#pragma once

#include "Math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace phazite::devices::dsp
{
// A curve: its value for an input, and the integral of that from 0 to the input (in double precision: ADAA divides the
// difference of two of them by the difference of the inputs, which may be small).
struct StraightCurve
{
    static float value (float x) noexcept { return x; }
    static double integral (double x) noexcept { return 0.5 * x * x; }
};

// softClip (Math.h): like tanh, exactly 1 from 3 on.
struct SoftCurve
{
    static float value (float x) noexcept { return softClip (x); }

    static double integral (double x) noexcept
    {
        const auto a = std::abs (x);

        if (a >= 3.0)
            return a - 2.5 + 4.0 / 3.0 * 1.3862943611198906; // ln 4

        const auto x2 = x * x;
        return x2 / 18.0 + 4.0 / 3.0 * std::log1p (x2 / 3.0);
    }
};

// x / (1 + |x|): rounds off earlier and more gently than soft.
struct TapeCurve
{
    static float value (float x) noexcept { return x / (1.0f + std::abs (x)); }

    static double integral (double x) noexcept
    {
        const auto a = std::abs (x);
        return a - std::log1p (a);
    }
};

// Clipping at 1.
struct HardCurve
{
    static float value (float x) noexcept { return std::clamp (x, -1.0f, 1.0f); }

    static double integral (double x) noexcept
    {
        const auto a = std::abs (x);
        return a <= 1.0 ? 0.5 * x * x : a - 0.5;
    }
};

// sin (pi x / 2): smooth up to 1, then folds back.
struct FoldCurve
{
    static float value (float x) noexcept { return (float) std::sin (1.5707963267948966 * (double) x); }
    static double integral (double x) noexcept { return 0.6366197723675814 * (1.0 - std::cos (1.5707963267948966 * x)); }
};

// softClip (x + x^2 / 5): lopsided, so even harmonics too (the DC it leaves is filtered out after). Its integral has no
// short closed form, so it comes from a table worked out once (the first time a curve of it is made, off the audio thread).
struct TubeCurve
{
    static float value (float x) noexcept { return softClip (x + 0.2f * x * x); }
    static double integral (double x) noexcept;

    // Works the table out, if it hasn't been yet: before the audio thread plays the curve.
    static void prepare();
};

// One channel's first-order ADAA of a curve, at the rate it is given, with its delay made up to a whole 4 samples by the
// filter that levels its top octave again. A gain before the curve, and its inverse after, can move from sample to sample
// (the curve itself stays the same); a change of curve starts the averaging again.
class AntialiasedCurve
{
public:
    static constexpr int delay = 4; // samples at its rate: half of one from the averaging, three and a half from the filter

    void reset() noexcept
    {
        last = lastIntegral = 0.0;
        started = false;
        history.fill (0.0f);
        newest = 0;
    }

    // A sample through a curve: one of those above, or any with value() and integral() (the multiband's, whose knee its
    // settings move).
    template <typename Curve>
    float process (float input, const Curve& curve) noexcept
    {
        const auto x = (double) input;
        const auto integral = curve.integral (x);

        if (! started)
        {
            last = x;
            lastIntegral = integral;
            started = true;
        }

        const auto difference = x - last;
        const auto averaged =
            std::abs (difference) > 1.0e-6 ? (float) ((integral - lastIntegral) / difference) : curve.value ((float) (0.5 * (x + last)));
        last = x;
        lastIntegral = integral;
        return level (averaged);
    }

    // After a change of curve (or of what feeds it, as a switch), the averaging starts again from the next sample.
    void restartAveraging() noexcept { started = false; }

private:
    double last = 0.0, lastIntegral = 0.0;
    bool started = false;

    // The levelling filter: an 8-tap linear-phase one, whose gain times the averaging's, cos (w / 2), is 1 - sin^8 (w / 2),
    // as flat as such a pair can be at 0 Hz (its taps make 1 / cos (w / 2) up to the cube of sin^2 (w / 2)).
    static constexpr std::array<float, 4> taps { -0.0078125f, 0.0703125f, -0.2890625f, 0.7265625f };
    std::array<float, 16> history {}; // the last 8 samples twice over, so that they are always in a row
    int newest = 0;

    float level (float sample) noexcept
    {
        newest = (newest + 1) & 7;
        history[(size_t) newest] = history[(size_t) newest + 8] = sample;
        const auto* last8 = history.data() + newest + 1; // oldest first
        return taps[0] * (last8[0] + last8[7]) + taps[1] * (last8[1] + last8[6]) + taps[2] * (last8[2] + last8[5])
               + taps[3] * (last8[3] + last8[4]);
    }
};
} // namespace phazite::devices::dsp
