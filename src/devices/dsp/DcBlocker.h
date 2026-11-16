// SPDX-License-Identifier: AGPL-3.0-or-later
// A DC blocker for an instrument's output: a 12 dB high-pass at 5 Hz (Butterworth), a state-variable filter (Cytomic's) in
// doubles, as its poles lie close to the unit circle. It takes away what lies under the notes, steady or wandering a few
// hertz at most: what ring modulation and FM leave at whole ratios, a soft clipper's offset of a lopsided wave. 20 Hz keeps
// its level within 0.02 dB (20.7 degrees of phase lead there). One for each channel. Real-time safe.
//
// It changes the waveform of anything it plays, a clean note's too: it takes out the net area a note has and puts it back
// as a slow tail, about 44 times that area a second at first, settling over some 45 ms. So an instrument plays it only
// where an offset can arise, and otherwise leaves its samples as they are, bit for bit. Taken on in mid-sound it starts at
// rest, passing the sound at first and settling, faded in over blendSeconds; let go, it fades out over as long and stops,
// so neither clicks. Once playback has stopped and no note sounds (quiet), what it still rings with (the low part of what
// it took away) fades out over quietSeconds, so that nothing sounds on after Stop.
#pragma once

#include <algorithm>
#include <cmath>

namespace phazite::devices::dsp
{
class DcBlocker
{
public:
    static constexpr double hertz = 5.0;
    static constexpr double blendSeconds = 0.005; // how long it fades in as it is taken on, and out as it is let go
    static constexpr double quietSeconds = 0.003; // how fast it fades out what it rings with once the notes are gone

    void prepare (double sampleRate) noexcept
    {
        const auto g = std::tan (3.14159265358979323846 * hertz / sampleRate);
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
        quietDecay = std::exp (-1.0 / (quietSeconds * sampleRate));
        blendStep = (float) std::min (1.0, 1.0 / (blendSeconds * sampleRate));
        reset();
    }

    void reset() noexcept
    {
        s1 = s2 = 0.0;
        blend = 0.0f;
    }

    // Whether it plays at all now, or is fading out.
    bool isInUse() const noexcept { return blend > 0.0f; }

    // One sample through the filter alone.
    float process (float x) noexcept
    {
        const auto v3 = (double) x - s2;
        const auto v1 = a1 * s1 + a2 * v3;
        const auto v2 = s2 + a2 * s1 + a3 * v3;
        s1 = 2.0 * v1 - s1;
        s2 = 2.0 * v2 - s2;
        return (float) ((double) x - k * v1 - v2);
    }

    // Samples in place: through it where it is wanted, faded in and out as it is taken on and let go; untouched where it
    // isn't and has faded out.
    void process (float* samples, int numSamples, bool wanted, bool quiet) noexcept
    {
        if (! wanted && blend <= 0.0f)
            return;

        for (int i = 0; i < numSamples; ++i)
        {
            const auto x = samples[i];
            const auto y = process (x);

            if (quiet)
            {
                s1 *= quietDecay;
                s2 *= quietDecay;
            }

            blend = wanted ? std::min (1.0f, blend + blendStep) : std::max (0.0f, blend - blendStep);
            samples[i] = x + blend * (y - x);
        }

        if (blend <= 0.0f)
            s1 = s2 = 0.0; // let go: at rest for when it is next taken on
    }

private:
    static constexpr double k = 1.4142135623730951; // 1 / Q: Butterworth
    double a1 = 1.0, a2 = 0.0, a3 = 0.0, s1 = 0.0, s2 = 0.0, quietDecay = 0.0;
    float blend = 0.0f, blendStep = 1.0f;
};
} // namespace phazite::devices::dsp
