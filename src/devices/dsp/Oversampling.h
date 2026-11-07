// SPDX-License-Identifier: AGPL-3.0-or-later
// Twice the sample rate, a sample at a time, for a curve inside a device's per-sample loop (the wavetable synth's drive):
// polyphase IIR half-band filters, two chains of all-pass sections that JUCE's FilterDesign designs as it does for
// juce::dsp::Oversampling at its best quality, each sample in becoming two and every two going back as one. Where a
// device works in blocks, juce::dsp::Oversampling does the same (the saturator's, the filter's). Real-time safe, once the
// filters are designed: the first oversampler made designs them, off the audio thread.
#pragma once

#include "Saturation.h"

#include <array>
#include <utility>

namespace phazite::devices::dsp
{
class Oversampler2x
{
public:
    Oversampler2x();

    void reset() noexcept
    {
        upState.fill (0.0f);
        downState.fill (0.0f);
        downDelay = 0.0f;
    }

    // A sample in, its two at twice the rate out.
    std::pair<float, float> up (float sample) noexcept
    {
        return { runChain (sample, halfBands->up, 0, halfBands->up.direct, upState),
                 runChain (sample, halfBands->up, halfBands->up.direct, halfBands->up.count, upState) };
    }

    // Two at twice the rate in, a sample out.
    float down (float first, float second) noexcept
    {
        const auto direct = runChain (first, halfBands->down, 0, halfBands->down.direct, downState);
        const auto out = 0.5f * (downDelay + direct);
        downDelay = runChain (second, halfBands->down, halfBands->down.direct, halfBands->down.count, downState);
        return out;
    }

    // A half-band filter's all-pass sections: the first `direct` for the direct path, the rest for the delayed one.
    struct HalfBand
    {
        static constexpr int most = 16;
        std::array<float, most> alphas {};
        int count = 0, direct = 0;
    };

    struct HalfBands
    {
        HalfBand up, down;
    };

private:
    const HalfBands* halfBands = nullptr;
    std::array<float, HalfBand::most> upState {}, downState {};
    float downDelay = 0.0f;

    static float runChain (float input, const HalfBand& band, int from, int to, std::array<float, HalfBand::most>& state) noexcept
    {
        for (int n = from; n < to; ++n)
        {
            const auto alpha = band.alphas[(size_t) n];
            const auto output = alpha * input + state[(size_t) n];
            state[(size_t) n] = input - alpha * output;
            input = output;
        }

        return input;
    }
};

// A curve at twice the rate, played by its antiderivative (Saturation.h), a sample at a time, a few samples late: the
// half-band filters' delay and the curve's 2.
class AntialiasedShaper
{
public:
    void reset() noexcept
    {
        oversampler.reset();
        curve.reset();
    }

    template <typename Curve>
    float process (float sample, const Curve& shape) noexcept
    {
        const auto [first, second] = oversampler.up (sample);
        const auto a = curve.process (first, shape);
        return oversampler.down (a, curve.process (second, shape));
    }

    // After a change of curve, its averaging starts again.
    void restartAveraging() noexcept { curve.restartAveraging(); }

    // How late it plays what it is given, in samples, at low frequencies: its half-band filters' delay and its curve's 2.
    static float getLatencySamples();

private:
    Oversampler2x oversampler;
    AntialiasedCurve curve;
};
} // namespace phazite::devices::dsp
