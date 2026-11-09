// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DrumSynthesis.h"

#include <algorithm>
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

Sound makeKick (double rate, const KickShape& shape)
{
    Sound sound (1, (int) (shape.length * rate));
    juce::Random random (shape.seed);
    juce::IIRFilter clickFilter, noiseFilter;
    clickFilter.setCoefficients (juce::IIRCoefficients::makeBandPass (rate, shape.clickHz, 0.8));
    noiseFilter.setCoefficients (juce::IIRCoefficients::makeBandPass (rate, shape.noiseHz, 0.7));
    auto phase = 0.0;

    for (int i = 0; i < sound.getNumSamples(); ++i)
    {
        const auto t = i / rate;
        auto hz = shape.endHz + (shape.startHz - shape.endHz) * std::exp (-t / shape.pitchSeconds);

        if (shape.chirpHz > 0.0)
            hz += (shape.chirpHz - shape.startHz) * std::exp (-t / shape.chirpSeconds);

        phase += twoPi * hz / rate;
        const auto level = t < shape.holdSeconds ? 1.0 : std::exp (-(t - shape.holdSeconds) / shape.decaySeconds);
        const auto body = std::sin (phase) * level * juce::jmin (1.0, t / 0.0005);
        const auto snap = shape.click * clickFilter.processSingleSampleRaw (noise (random)) * std::exp (-t / shape.clickSeconds);
        const auto breath = shape.noise * noiseFilter.processSingleSampleRaw (noise (random)) * std::exp (-t / shape.noiseSeconds);
        sound.setSample (0, i, softClip (body + snap + breath, shape.drive));
    }

    return sound;
}

Sound makeSnare (double rate, const SnareShape& shape)
{
    Sound sound (1, (int) (shape.length * rate));
    juce::Random random (shape.seed);
    juce::IIRFilter highPass, smooth;
    highPass.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, shape.snaresLowCut, 0.707));
    smooth.setCoefficients (juce::IIRCoefficients::makeLowPass (rate, shape.snaresHighCut, 0.707));

    for (int i = 0; i < sound.getNumSamples(); ++i)
    {
        const auto t = i / rate;
        const auto shell = shape.lowLevel * std::sin (twoPi * shape.lowHz * t) * std::exp (-t / shape.lowSeconds)
                           + shape.highLevel * std::sin (twoPi * shape.highHz * t) * std::exp (-t / shape.highSeconds);
        const auto snares = shape.snaresLevel * smooth.processSingleSampleRaw (highPass.processSingleSampleRaw (noise (random)))
                            * std::exp (-t / shape.snaresSeconds);
        sound.setSample (0, i, softClip ((shell + snares) * juce::jmin (1.0, t / 0.0005), shape.drive));
    }

    return sound;
}

Sound makeClap (double rate, const ClapShape& shape)
{
    Sound sound (1, (int) (shape.length * rate));
    juce::Random random (shape.seed);
    juce::IIRFilter bandPass, highPass;
    bandPass.setCoefficients (juce::IIRCoefficients::makeBandPass (rate, shape.centreHz, shape.q));
    highPass.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, shape.lowCut, 0.707));
    const std::array<double, 4> strikes { 0.0, 0.0105 * shape.spread, 0.0205 * shape.spread, 0.031 * shape.spread };

    for (int i = 0; i < sound.getNumSamples(); ++i)
    {
        const auto t = i / rate;
        auto envelope = t >= strikes.back() ? shape.tailLevel * std::exp (-(t - strikes.back()) / shape.tailSeconds) : 0.0;

        for (const auto strike : strikes)
            if (t >= strike)
                envelope = juce::jmax (envelope, std::exp (-(t - strike) / 0.0045));

        sound.setSample (0, i, (float) (envelope * highPass.processSingleSampleRaw (bandPass.processSingleSampleRaw (noise (random)))));
    }

    return sound;
}

Sound makeHat (double rate, const HatShape& shape)
{
    static constexpr std::array<double, 6> frequencies { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };
    Sound sound (1, (int) (shape.length * rate));
    juce::Random random (shape.seed);
    juce::IIRFilter bandPass, highPass;
    bandPass.setCoefficients (juce::IIRCoefficients::makeBandPass (rate, shape.centreHz, 1.0));
    highPass.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, shape.lowCut, 0.707));

    for (int i = 0; i < sound.getNumSamples(); ++i)
    {
        const auto t = i / rate;
        auto metal = 0.0;

        for (const auto frequency : frequencies)
            metal += std::fmod (t * (frequency * shape.pitch), 1.0) < 0.5 ? 1.0 : -1.0;

        const auto x =
            highPass.processSingleSampleRaw (bandPass.processSingleSampleRaw ((float) (metal / 6.0 + shape.noise * noise (random))));
        sound.setSample (0, i, (float) (x * std::exp (-t / shape.decaySeconds)));
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

Sound makeStereo (const Sound& left, const Sound& right)
{
    Sound sound (2, std::max (left.getNumSamples(), right.getNumSamples()));
    sound.clear();
    sound.copyFrom (0, 0, left, 0, 0, left.getNumSamples());
    sound.copyFrom (1, 0, right, 0, 0, right.getNumSamples());
    return sound;
}

void makeDusty (Sound& sound, double rate, const DustShape& shape)
{
    const auto step = shape.heldRate / rate;
    const auto levels = std::pow (2.0, shape.bits - 1);

    for (int channel = 0; channel < sound.getNumChannels(); ++channel)
    {
        std::array<juce::IIRFilter, 2> lowPasses;

        for (auto& filter : lowPasses)
            filter.setCoefficients (juce::IIRCoefficients::makeLowPass (rate, shape.cutoffHz, 0.707));

        auto* samples = sound.getWritePointer (channel);
        auto phase = 1.0; // a new reading at the first sample
        auto held = 0.0f;

        for (int i = 0; i < sound.getNumSamples(); ++i)
        {
            // A reading each 1 / heldRate seconds, rounded to the bits it has, held until the next.
            if ((phase += step) >= 1.0)
            {
                phase -= 1.0;
                held = (float) (std::round (samples[i] * levels) / levels);
            }

            auto x = held;

            for (auto& filter : lowPasses)
                x = filter.processSingleSampleRaw (x);

            samples[i] = softClip (x, shape.drive);
        }
    }
}
} // namespace phazite::devices::drums
