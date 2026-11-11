// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DrumSynthesis.h"
#include "devices/dsp/Filters.h"

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
    constexpr double pi = juce::MathConstants<double>::pi;

    // A level held until hold, then falling with tau: a gate's, or a VCA's that waits before it lets go.
    double holdThenFall (double t, double hold, double tau)
    {
        return t < hold ? 1.0 : std::exp (-(t - hold) / tau);
    }
} // namespace

float noise (juce::Random& random)
{
    return random.nextFloat() * 2.0f - 1.0f;
}

float noise (dsp::Random& random)
{
    return random.nextBipolar();
}

dsp::Random makeRandom (int seed, int stream)
{
    // The seed and the stream stirred together (the finalizer of MurmurHash3, Austin Appleby's, in the public domain), so
    // that seeds or streams next to each other start generators far apart; then a few numbers drawn and dropped, as a
    // xorshift's first ones still show its seed.
    auto state = (std::uint32_t) seed * 0x9e3779b9u + (std::uint32_t) stream * 0x85ebca6bu + 0x165667b1u;
    state ^= state >> 16;
    state *= 0x85ebca6bu;
    state ^= state >> 13;
    state *= 0xc2b2ae35u;
    state ^= state >> 16;
    dsp::Random random (state);

    for (int i = 0; i < 8; ++i)
        random.nextInt();

    return random;
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
    const auto numSamples = sound.getNumSamples();

    // The body first, whole, as its equalizer needs it before the click and the drive.
    std::vector<double> body ((size_t) numSamples);
    auto phase = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto t = i / rate;
        auto hz = shape.endHz + (shape.startHz - shape.endHz) * std::exp (-t / shape.pitchSeconds);

        if (shape.chirpHz > 0.0)
            hz += (shape.chirpHz - shape.startHz) * std::exp (-t / shape.chirpSeconds);

        phase += twoPi * hz / rate;
        const auto level = t < shape.holdSeconds ? 1.0 : std::exp (-(t - shape.holdSeconds) / shape.decaySeconds);
        body[(size_t) i] = std::sin (phase) * level * (shape.attackSeconds > 0.0 ? juce::jmin (1.0, t / shape.attackSeconds) : 1.0);
    }

    if (std::abs (shape.boostDecibels) > 0.0 || std::abs (shape.shelfDecibels) > 0.0)
    {
        // The bands of Phazite's EQ (dsp/Filters.h), which stay exact this low; then the body back at a peak of 1.
        const auto bell = dsp::makeFilter (dsp::FilterShape::bell, shape.boostHz, rate, shape.boostQ, shape.boostDecibels);
        const auto shelf = dsp::makeFilter (dsp::FilterShape::lowShelf, shape.shelfHz, rate, 0.7071, shape.shelfDecibels);
        dsp::Filter bellFilter, shelfFilter;
        auto peak = 0.0;

        for (auto& sample : body)
        {
            sample = shelfFilter.process (bellFilter.process ((float) sample, bell), shelf);
            peak = std::max (peak, std::abs (sample));
        }

        if (peak > 0.0)
            for (auto& sample : body)
                sample /= peak;
    }

    juce::Random random (shape.seed);
    juce::IIRFilter clickFilter, noiseFilter;
    clickFilter.setCoefficients (juce::IIRCoefficients::makeBandPass (rate, shape.clickHz, shape.clickQ));
    noiseFilter.setCoefficients (juce::IIRCoefficients::makeBandPass (rate, shape.noiseHz, 0.7));
    Ringing knock ({ .hz = shape.clickHz, .tau = shape.clickQ / (pi * shape.clickHz) }, rate); // the band, struck once

    for (int i = 0; i < numSamples; ++i)
    {
        const auto t = i / rate;
        const auto hiss = clickFilter.processSingleSampleRaw (noise (random));
        const auto struck = shape.clickKnock > 0.0 ? (1.0 - shape.clickKnock) * hiss + shape.clickKnock * knock.at (t) : (double) hiss;
        const auto snap = shape.click * struck * std::exp (-t / shape.clickSeconds);
        const auto breath = shape.noise * noiseFilter.processSingleSampleRaw (noise (random)) * std::exp (-t / shape.noiseSeconds);
        sound.setSample (0, i, softClip (body[(size_t) i] + snap + breath, shape.drive));
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
    std::vector<Ringing> modes;

    for (const auto& mode : shape.modes)
        modes.emplace_back (mode, rate);

    for (int i = 0; i < sound.getNumSamples(); ++i)
    {
        const auto t = i / rate;
        auto shell = shape.lowLevel * std::sin (twoPi * shape.lowHz * t) * std::exp (-t / shape.lowSeconds)
                     + shape.highLevel * std::sin (twoPi * shape.highHz * t) * std::exp (-t / shape.highSeconds);

        for (auto& mode : modes)
            shell += mode.at (t);

        const auto snares = shape.snaresLevel * smooth.processSingleSampleRaw (highPass.processSingleSampleRaw (noise (random)))
                            * std::exp (-t / shape.snaresSeconds);
        sound.setSample (0, i, softClip ((shell + snares) * juce::jmin (1.0, t / 0.0005), shape.drive));
    }

    return sound;
}

Sound makeClap (double rate, const ClapShape& shape)
{
    const auto wide = shape.width > 0.0;
    Sound sound (wide ? 2 : 1, (int) (shape.length * rate));
    juce::Random random (shape.seed);
    juce::IIRFilter bandPass, highPass;
    bandPass.setCoefficients (juce::IIRCoefficients::makeBandPass (rate, shape.centreHz, shape.q));
    highPass.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, shape.lowCut, 0.707));

    // When each hand strikes: the 808's four, and where a fifth and a sixth fall after them.
    const std::array<double, 6> all {
        0.0, 0.0105 * shape.spread, 0.0205 * shape.spread, 0.031 * shape.spread, 0.0375 * shape.spread, 0.0445 * shape.spread
    };
    const auto numStrikes = (size_t) juce::jlimit (1, (int) all.size(), shape.strikes);
    const auto last = all[numStrikes - 1];

    // Each side's own noise, through the band as what both sides hear is.
    struct Side
    {
        dsp::Random generator;
        juce::IIRFilter bandPass, highPass;
    };

    std::array<Side, 2> sides { Side { makeRandom (shape.seed, 1), bandPass, highPass },
                                Side { makeRandom (shape.seed, 2), bandPass, highPass } };

    for (int i = 0; i < sound.getNumSamples(); ++i)
    {
        const auto t = i / rate;
        auto envelope = t >= last ? shape.tailLevel * std::exp (-(t - last) / shape.tailSeconds) : 0.0;

        for (size_t strike = 0; strike < numStrikes; ++strike)
            if (t >= all[strike])
                envelope = juce::jmax (envelope, std::exp (-(t - all[strike]) / 0.0045));

        const auto shared = highPass.processSingleSampleRaw (bandPass.processSingleSampleRaw (noise (random)));

        if (! wide)
        {
            sound.setSample (0, i, (float) (envelope * shared));
            continue;
        }

        for (size_t side = 0; side < sides.size(); ++side)
        {
            auto& own = sides[side];
            const auto apart = own.highPass.processSingleSampleRaw (own.bandPass.processSingleSampleRaw (noise (own.generator)));
            sound.setSample ((int) side, i, (float) (envelope * (shared + shape.width * apart)));
        }
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
    highPass.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, shape.lowCut, shape.lowCutQ));
    const auto isCut = shape.fadeSeconds > 0.0;

    for (int i = 0; i < sound.getNumSamples(); ++i)
    {
        const auto t = i / rate;
        auto metal = 0.0;

        for (const auto frequency : frequencies)
            metal += std::fmod (t * (frequency * shape.pitch), 1.0) < 0.5 ? 1.0 : -1.0;

        const auto x =
            highPass.processSingleSampleRaw (bandPass.processSingleSampleRaw ((float) (metal / 6.0 + shape.noise * noise (random))));
        auto level = std::exp (-t / shape.decaySeconds);

        if (shape.attackSeconds > 0.0)
            level *= std::min (1.0, t / shape.attackSeconds);

        // Cut: down over the fade, a half cosine, and nothing after.
        if (isCut && t >= shape.holdSeconds)
            level *=
                t < shape.holdSeconds + shape.fadeSeconds ? 0.5 * (1.0 + std::cos (pi * (t - shape.holdSeconds) / shape.fadeSeconds)) : 0.0;

        sound.setSample (0, i, (float) (x * level));
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

Sound makeStruck (double rate, const StruckShape& shape)
{
    Sound sound (1, (int) (shape.length * rate));
    auto random = makeRandom (shape.seed);
    std::vector<Ringing> partials;

    for (const auto& partial : shape.partials)
        partials.emplace_back (partial, rate);

    juce::IIRFilter band;
    std::array<juce::IIRFilter, 2> top; // two sections in a row: a Butterworth slope of 24 dB an octave
    band.setCoefficients (juce::IIRCoefficients::makeBandPass (rate, std::min (shape.clickHz, 0.45 * rate), shape.clickQ));

    for (size_t section = 0; section < top.size(); ++section)
        top[section].setCoefficients (
            juce::IIRCoefficients::makeLowPass (rate, std::min (shape.clickHighCut, 0.45 * rate), dsp::getButterworthQ (4, (int) section)));

    for (int i = 0; i < sound.getNumSamples(); ++i)
    {
        const auto t = i / rate;
        auto rung = 0.0;

        for (auto& partial : partials)
            rung += partial.at (t);

        auto hiss = band.processSingleSampleRaw (noise (random));

        for (auto& section : top)
            hiss = section.processSingleSampleRaw (hiss);

        const auto struck = rung + shape.click * hiss * holdThenFall (t, shape.clickHoldSeconds, shape.clickSeconds);
        sound.setSample (0, i, shape.drive > 0.0 ? softClip (struck, shape.drive) : (float) struck);
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

Sound addRoom (const Sound& drum, double rate, const RoomShape& shape)
{
    // A kit's sound fades over its last 30 %: the tail is 60 dB down, at its low end, before that.
    const auto preDelay = std::max (0, (int) std::lround (shape.preDelaySeconds * rate));
    const auto onset = std::max (1, (int) std::lround (shape.onsetSeconds * rate));
    const auto length = std::max (drum.getNumSamples(), (int) ((shape.preDelaySeconds + shape.decaySeconds) * rate / 0.7));
    auto order = 1;

    while ((1 << order) < drum.getNumSamples() + length)
        ++order;

    const auto size = (size_t) 1 << order;
    const juce::dsp::FFT fft (order);

    // The room's answer to a click, each side's: white noise taken apart into short overlapping frames (about 20 ms, a
    // quarter apart, under a Hann window going in and another coming out), each frame's every frequency turned down by
    // what it has fallen by then and by the room's band, and the frames put back together. So the fall is smooth in time
    // and in frequency, as a room's is, with no bands to cross over.
    auto frameOrder = 6;

    while ((1 << frameOrder) < (int) (0.02 * rate))
        ++frameOrder;

    const juce::dsp::FFT frameFft (frameOrder);
    const auto frameSize = 1 << frameOrder, hop = frameSize / 4;
    std::vector<float> window ((size_t) frameSize), frame ((size_t) frameSize * 2);

    for (int i = 0; i < frameSize; ++i)
        window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (twoPi * i / frameSize));

    // What each frequency falls by in a second, in decibels, and the band's gain there (12 dB an octave at each end).
    const auto lowRate = 60.0 / shape.decaySeconds, highRate = 60.0 / (shape.decaySeconds * shape.highDecay);
    std::vector<double> fallRates ((size_t) frameSize / 2 + 1), bandGains ((size_t) frameSize / 2 + 1);

    for (size_t bin = 0; bin < fallRates.size(); ++bin)
    {
        const auto hz = (double) bin * rate / frameSize;
        fallRates[bin] = lowRate + (highRate - lowRate) * hz / (hz + shape.dampingHz);
        const auto low = std::pow (hz / shape.lowCut, 2.0), high = std::pow (hz / shape.highCut, 2.0);
        bandGains[bin] = low / std::sqrt (1.0 + low * low) / std::sqrt (1.0 + high * high);
    }

    // Each side's noise: its own, and what both share where the room is narrower.
    const auto own = std::sqrt (juce::jlimit (0.0, 1.0, shape.width)), shared = std::sqrt (1.0 - own * own);
    auto both = makeRandom (shape.seed, 0);
    std::vector<float> sharedNoise ((size_t) (length + frameSize), 0.0f);

    if (shared > 0.0)
        for (auto& sample : sharedNoise)
            sample = noise (both);

    std::array<std::vector<float>, 2> rooms;

    for (size_t side = 0; side < rooms.size(); ++side)
    {
        auto random = makeRandom (shape.seed, (int) side + 1);
        std::vector<float> white (sharedNoise.size());

        for (size_t i = 0; i < white.size(); ++i)
            white[i] = (float) (own * noise (random) + shared * sharedNoise[i]);

        auto& room = rooms[side];
        room.assign (size * 2, 0.0f);

        // The frames start a frame early, so that the room's first samples have their four frames over them too.
        for (auto start = hop - frameSize; start < length; start += hop)
        {
            std::fill (frame.begin(), frame.end(), 0.0f);

            for (int i = 0; i < frameSize; ++i)
                if (const auto at = start + i; at >= 0)
                    frame[(size_t) i] = white[(size_t) at] * window[(size_t) i];

            frameFft.performRealOnlyForwardTransform (frame.data(), true);
            const auto seconds = std::max (0.0, (start + frameSize / 2 - preDelay) / rate); // since the room answered

            for (size_t bin = 0; bin < fallRates.size(); ++bin)
            {
                const auto gain = (float) (bandGains[bin] * std::pow (10.0, -fallRates[bin] * seconds / 20.0));
                frame[2 * bin] *= gain;
                frame[2 * bin + 1] *= gain;
            }

            frameFft.performRealOnlyInverseTransform (frame.data());

            // A Hann window going in and another coming out, a quarter of a frame apart, add up to 1.5.
            for (int i = 0; i < frameSize; ++i)
                if (const auto at = start + i; at >= 0 && at < length)
                    room[(size_t) at] += frame[(size_t) i] * window[(size_t) i] / 1.5f;
        }

        // Nothing before the pre-delay, then in over the onset, a half cosine.
        for (int i = 0; i < std::min (length, preDelay + onset); ++i)
            room[(size_t) i] *= i < preDelay ? 0.0f : (float) (0.5 * (1.0 - std::cos (pi * (i - preDelay) / onset)));
    }

    // The drum played into each side's room (both its sides as one), so that the room has the drum's colour.
    std::vector<float> drumSpectrum (size * 2, 0.0f);

    for (int channel = 0; channel < drum.getNumChannels(); ++channel)
        for (int i = 0; i < drum.getNumSamples(); ++i)
            drumSpectrum[(size_t) i] += drum.getSample (channel, i) / (float) drum.getNumChannels();

    fft.performRealOnlyForwardTransform (drumSpectrum.data());
    auto loudest = 0.0f;

    for (auto& room : rooms)
    {
        fft.performRealOnlyForwardTransform (room.data());

        for (size_t bin = 0; bin < size; ++bin)
        {
            const std::complex<float> product = std::complex<float> (drumSpectrum[2 * bin], drumSpectrum[2 * bin + 1])
                                                * std::complex<float> (room[2 * bin], room[2 * bin + 1]);
            room[2 * bin] = product.real();
            room[2 * bin + 1] = product.imag();
        }

        fft.performRealOnlyInverseTransform (room.data());

        for (int i = 0; i < length; ++i)
            loudest = std::max (loudest, std::abs (room[(size_t) i]));
    }

    Sound sound (2, length);
    sound.clear();

    for (int side = 0; side < 2; ++side)
        sound.copyFrom (side, 0, drum, std::min (side, drum.getNumChannels() - 1), 0, drum.getNumSamples());

    const auto gain = loudest > 0.0f ? (float) shape.level * drum.getMagnitude (0, drum.getNumSamples()) / loudest : 0.0f;

    for (int side = 0; side < 2; ++side)
        for (int i = 0; i < length; ++i)
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
        const auto tone =
            triangle
            * (shape.holdSeconds > 0.0 ? holdThenFall (t, shape.holdSeconds, shape.toneSeconds) : std::exp (-t / shape.toneSeconds));

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
