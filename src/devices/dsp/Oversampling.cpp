// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Oversampling.h"

#include <juce_dsp/juce_dsp.h>

#include <vector>

namespace phazite::devices::dsp
{
namespace
{
    // The all-pass sections JUCE designs for a half-band filter, ordered as juce::dsp::Oversampling orders them: the direct
    // path's, then the delayed path's but its first (the delay itself), the direct path taking the larger half.
    Oversampler2x::HalfBand design (float transitionWidth, float stopbandDecibels)
    {
        const auto structure =
            juce::dsp::FilterDesign<float>::designIIRLowpassHalfBandPolyphaseAllpassMethod (transitionWidth, stopbandDecibels);
        std::vector<float> alphas;

        for (int i = 0; i < structure.directPath.size(); ++i)
            alphas.push_back (structure.directPath.getObjectPointer (i)->coefficients[0]);

        for (int i = 1; i < structure.delayedPath.size(); ++i)
            alphas.push_back (structure.delayedPath.getObjectPointer (i)->coefficients[0]);

        Oversampler2x::HalfBand band;
        band.count = std::min ((int) alphas.size(), Oversampler2x::HalfBand::most);
        band.direct = band.count - band.count / 2;
        std::copy (alphas.begin(), alphas.begin() + band.count, band.alphas.begin());
        return band;
    }

    const Oversampler2x::HalfBands& getHalfBands()
    {
        // juce::dsp::Oversampling's first stage at its best quality.
        static const Oversampler2x::HalfBands halfBands { design (0.05f, -90.0f), design (0.06f, -75.0f) };
        return halfBands;
    }
} // namespace

Oversampler2x::Oversampler2x()
    : halfBands (&getHalfBands())
{
}

float AntialiasedShaper::getLatencySamples()
{
    static const float latency = []
    {
        // The half-band filters' delay at 0 Hz, the centre of their impulse response's weight; and the curve's.
        Oversampler2x oversampler;
        auto weight = 0.0, moment = 0.0;

        for (int n = 0; n < 4096; ++n)
        {
            const auto [first, second] = oversampler.up (n == 0 ? 1.0f : 0.0f);
            const auto out = (double) oversampler.down (first, second);
            weight += out;
            moment += n * out;
        }

        return (float) (moment / weight) + 0.5f * (float) AntialiasedCurve::delay;
    }();

    return latency;
}
} // namespace phazite::devices::dsp
