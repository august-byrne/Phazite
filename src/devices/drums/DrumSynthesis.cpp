// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DrumSynthesis.h"

#include <array>
#include <cmath>

namespace phazite::devices::drums
{
namespace
{
    constexpr double twoPi = juce::MathConstants<double>::twoPi;
} // namespace

float noise (juce::Random& random)
{
    return random.nextFloat() * 2.0f - 1.0f;
}

float softClip (double x, double drive)
{
    return (float) (std::tanh (drive * x) / std::tanh (drive));
}

Sound makeKick (double rate, double startHz, double endHz, double pitchSeconds, double decaySeconds, double length, double click,
                double drive)
{
    Sound sound (1, (int) (length * rate));
    juce::Random random (1);
    juce::IIRFilter clickFilter;
    clickFilter.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, 2000.0, 0.707));
    auto phase = 0.0;

    for (int i = 0; i < sound.getNumSamples(); ++i)
    {
        const auto t = i / rate;
        phase += twoPi * (endHz + (startHz - endHz) * std::exp (-t / pitchSeconds)) / rate;
        const auto body = std::sin (phase) * std::exp (-t / decaySeconds) * juce::jmin (1.0, t / 0.0005);
        const auto snap = click * clickFilter.processSingleSampleRaw (noise (random)) * std::exp (-t / 0.0025);
        sound.setSample (0, i, softClip (body + snap, drive));
    }

    return sound;
}

Sound makeSnare (double rate, double lowHz, double highHz, double snaresSeconds)
{
    Sound sound (1, (int) (0.45 * rate));
    juce::Random random (2);
    juce::IIRFilter highPass, smooth;
    highPass.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, 800.0, 0.707));
    smooth.setCoefficients (juce::IIRCoefficients::makeLowPass (rate, 9000.0, 0.707));

    for (int i = 0; i < sound.getNumSamples(); ++i)
    {
        const auto t = i / rate;
        const auto shell =
            0.6 * std::sin (twoPi * lowHz * t) * std::exp (-t / 0.07) + 0.4 * std::sin (twoPi * highHz * t) * std::exp (-t / 0.045);
        const auto snares =
            0.55 * smooth.processSingleSampleRaw (highPass.processSingleSampleRaw (noise (random))) * std::exp (-t / snaresSeconds);
        sound.setSample (0, i, softClip ((shell + snares) * juce::jmin (1.0, t / 0.0005), 1.6));
    }

    return sound;
}

Sound makeClap (double rate)
{
    Sound sound (1, (int) (0.55 * rate));
    juce::Random random (3);
    juce::IIRFilter bandPass, highPass;
    bandPass.setCoefficients (juce::IIRCoefficients::makeBandPass (rate, 1100.0, 1.3));
    highPass.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, 500.0, 0.707));
    constexpr std::array<double, 4> strikes { 0.0, 0.0105, 0.0205, 0.031 };

    for (int i = 0; i < sound.getNumSamples(); ++i)
    {
        const auto t = i / rate;
        auto envelope = t >= strikes.back() ? std::exp (-(t - strikes.back()) / 0.13) : 0.0;

        for (const auto strike : strikes)
            if (t >= strike)
                envelope = juce::jmax (envelope, std::exp (-(t - strike) / 0.0045));

        sound.setSample (0, i, (float) (envelope * highPass.processSingleSampleRaw (bandPass.processSingleSampleRaw (noise (random)))));
    }

    return sound;
}

Sound makeHat (double rate, double decaySeconds, double length)
{
    static constexpr std::array<double, 6> frequencies { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };
    Sound sound (1, (int) (length * rate));
    juce::Random random (4);
    juce::IIRFilter bandPass, highPass;
    bandPass.setCoefficients (juce::IIRCoefficients::makeBandPass (rate, 7100.0, 1.0));
    highPass.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, 6000.0, 0.707));

    for (int i = 0; i < sound.getNumSamples(); ++i)
    {
        const auto t = i / rate;
        auto metal = 0.0;

        for (const auto frequency : frequencies)
            metal += std::fmod (t * frequency, 1.0) < 0.5 ? 1.0 : -1.0;

        const auto x = highPass.processSingleSampleRaw (bandPass.processSingleSampleRaw ((float) (metal / 6.0 + 0.15 * noise (random))));
        sound.setSample (0, i, (float) (x * std::exp (-t / decaySeconds)));
    }

    return sound;
}

Sound makeCowbell (double rate)
{
    Sound sound (1, (int) (0.5 * rate));
    juce::IIRFilter bandPass;
    bandPass.setCoefficients (juce::IIRCoefficients::makeBandPass (rate, 2640.0, 1.5));

    for (int i = 0; i < sound.getNumSamples(); ++i)
    {
        const auto t = i / rate;
        const auto tone = (std::fmod (t * 540.0, 1.0) < 0.5 ? 0.5 : -0.5) + (std::fmod (t * 800.0, 1.0) < 0.5 ? 0.5 : -0.5);
        const auto envelope = 0.7 * std::exp (-t / 0.006) + 0.3 * std::exp (-t / 0.11);
        sound.setSample (0, i, (float) (envelope * bandPass.processSingleSampleRaw ((float) tone)));
    }

    return sound;
}
} // namespace phazite::devices::drums
