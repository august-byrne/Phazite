// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DrumSynthesis.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

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

Sound addGatedReverb (const Sound& drum, double rate, const GatedReverbShape& shape)
{
    const auto onset = std::max (1, (int) std::lround (shape.onsetSeconds * rate));
    const auto gate = std::max (1, (int) std::lround (shape.gateSeconds * rate));
    const auto close = std::max (1, (int) std::lround (shape.closeSeconds * rate));
    const auto shut = onset + gate + close;                                   // where the gate has shut, from the hit
    const auto length = std::max (drum.getNumSamples(), (int) (shut / 0.65)); // a kit's sound fades over its last 30 %
    auto order = 1;

    while ((1 << order) < drum.getNumSamples() + shut)
        ++order;

    const auto size = (size_t) 1 << order;
    const juce::dsp::FFT fft (order);

    // The drum's spectrum, both its sides as one; each side's room is the drum played through dense noise in the room's
    // band, falling slowly, so that the room has the drum's colour.
    std::vector<float> drumSpectrum (size * 2, 0.0f);

    for (int channel = 0; channel < drum.getNumChannels(); ++channel)
        for (int i = 0; i < drum.getNumSamples(); ++i)
            drumSpectrum[(size_t) i] += drum.getSample (channel, i) / (float) drum.getNumChannels();

    fft.performRealOnlyForwardTransform (drumSpectrum.data());

    // The gate, opened by the hit: in over the onset, held, then shut, each a half cosine.
    const auto gateAt = [&] (int i)
    {
        if (i < onset)
            return 0.5 * (1.0 - std::cos (juce::MathConstants<double>::pi * i / onset));

        if (i < onset + gate)
            return 1.0;

        return i < shut ? 0.5 * (1.0 + std::cos (juce::MathConstants<double>::pi * (i - onset - gate) / close)) : 0.0;
    };

    juce::Random random (shape.seed);
    std::array<std::vector<float>, 2> rooms;
    auto loudest = 0.0f;

    for (auto& room : rooms)
    {
        room.assign (size * 2, 0.0f);
        juce::IIRFilter lowCut, highCut;
        lowCut.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, shape.lowCut, 0.707));
        highCut.setCoefficients (juce::IIRCoefficients::makeLowPass (rate, shape.highCut, 0.707));

        for (int i = 0; i < shut; ++i)
        {
            const auto level = std::pow (10.0, -shape.fallDecibels * i / (onset + gate) / 20.0);
            room[(size_t) i] = (float) level * highCut.processSingleSampleRaw (lowCut.processSingleSampleRaw (noise (random)));
        }

        fft.performRealOnlyForwardTransform (room.data());

        for (size_t bin = 0; bin < size; ++bin)
        {
            const std::complex<float> product = std::complex<float> (drumSpectrum[2 * bin], drumSpectrum[2 * bin + 1])
                                                * std::complex<float> (room[2 * bin], room[2 * bin + 1]);
            room[2 * bin] = product.real();
            room[2 * bin + 1] = product.imag();
        }

        fft.performRealOnlyInverseTransform (room.data());

        for (int i = 0; i < shut; ++i)
        {
            room[(size_t) i] *= (float) gateAt (i);
            loudest = std::max (loudest, std::abs (room[(size_t) i]));
        }
    }

    Sound sound (2, length);
    sound.clear();

    for (int side = 0; side < 2; ++side)
        sound.copyFrom (side, 0, drum, std::min (side, drum.getNumChannels() - 1), 0, drum.getNumSamples());

    const auto gain = loudest > 0.0f ? (float) shape.level * drum.getMagnitude (0, drum.getNumSamples()) / loudest : 0.0f;

    for (int side = 0; side < 2; ++side)
        for (int i = 0; i < shut; ++i)
            sound.addSample (side, i, gain * rooms[(size_t) side][(size_t) i]);

    return sound;
}

Sound makeSimmons (double rate, const SimmonsShape& shape)
{
    Sound sound (1, (int) (shape.length * rate));
    juce::Random random (shape.seed);
    juce::IIRFilter clickFilter;
    clickFilter.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, 2000.0, 0.707));
    std::array<double, 4> poles {}; // the noise's four-pole low-pass, each pole a one-pole filter
    auto phase = 0.0;

    for (int i = 0; i < sound.getNumSamples(); ++i)
    {
        const auto t = i / rate;
        phase += (shape.endHz + (shape.startHz - shape.endHz) * std::exp (-t / shape.bendSeconds)) / rate;
        const auto triangle = 4.0 * std::abs (phase - std::floor (phase) - 0.5) - 1.0;
        const auto tone = triangle * std::exp (-t / shape.toneSeconds);

        const auto cutoff = shape.noiseToHz + (shape.noiseFromHz - shape.noiseToHz) * std::exp (-t / shape.sweepSeconds);
        const auto step = 1.0 - std::exp (-juce::MathConstants<double>::twoPi * cutoff / rate);
        double hiss = noise (random);

        for (auto& pole : poles)
            hiss = pole += step * (hiss - pole);

        // The four poles take the noise's level down a good deal; made up so that the share of noise is what it says.
        const auto breath = 3.0 * hiss * std::exp (-t / shape.noiseSeconds);
        const auto snap = shape.click * clickFilter.processSingleSampleRaw (noise (random)) * std::exp (-t / 0.0004);
        const auto onset = juce::jmin (1.0, t / 0.0005);
        sound.setSample (0, i, softClip (onset * ((1.0 - shape.noise) * tone + shape.noise * breath) + snap, shape.drive));
    }

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
            // A reading each 1 / heldRate seconds, rounded to the bits it has, held until the next; companded, rounded as
            // mu-law (mu 255) codes it, its steps finer the quieter it is.
            if ((phase += step) >= 1.0)
            {
                phase -= 1.0;

                if (shape.muLaw)
                {
                    constexpr double mu = 255.0;
                    const auto x = (double) juce::jlimit (-1.0f, 1.0f, samples[i]);
                    const auto coded = std::round (std::log1p (mu * std::abs (x)) / std::log1p (mu) * levels) / levels;
                    held = (float) std::copysign ((std::pow (1.0 + mu, coded) - 1.0) / mu, x);
                }
                else
                {
                    held = (float) (std::round (samples[i] * levels) / levels);
                }
            }

            auto x = held;

            for (auto& filter : lowPasses)
                x = filter.processSingleSampleRaw (x);

            samples[i] = softClip (x, shape.drive);
        }
    }
}
} // namespace phazite::devices::drums
