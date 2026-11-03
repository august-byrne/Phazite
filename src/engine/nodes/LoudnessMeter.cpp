// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LoudnessMeter.h"

#include <algorithm>
#include <cmath>

namespace phazite::engine
{
namespace
{
    constexpr float none = -1000.0f, floorLufs = -100.0f;

    // Loudness from a K-weighted power, as BS.1770 reckons it.
    double toLufs (double power) noexcept
    {
        return power > 0.0 ? std::max ((double) floorLufs, -0.691 + 10.0 * std::log10 (power)) : (double) floorLufs;
    }
} // namespace

void LoudnessMeter::prepare (double rate, int blockSize)
{
    sampleRate = rate;
    maximumBlockSize = std::max (1, blockSize);
    blockLength = std::max (1, (int) std::lround (rate * 0.1));

    // The K-weighting at this rate, as BS.1770's analogue filters through the bilinear transform: a shelf of about +4 dB
    // above 1.7 kHz, the head's, then a high-pass at 38 Hz. The analogue filters' parameters are libebur128's (MIT:
    // docs/third-party.md), and so is the gating's histogram of 0.1 LU bins.
    const auto pi = juce::MathConstants<double>::pi;

    for (auto& channel : filters)
    {
        auto& shelf = channel[0];
        const auto shelfK = std::tan (pi * 1681.974450955533 / rate);
        const auto shelfQ = 0.7071752369554196;
        const auto vh = std::pow (10.0, 3.999843853973347 / 20.0);
        const auto vb = std::pow (vh, 0.4996667741545416);
        const auto shelfA0 = 1.0 + shelfK / shelfQ + shelfK * shelfK;
        shelf.b0 = (vh + vb * shelfK / shelfQ + shelfK * shelfK) / shelfA0;
        shelf.b1 = 2.0 * (shelfK * shelfK - vh) / shelfA0;
        shelf.b2 = (vh - vb * shelfK / shelfQ + shelfK * shelfK) / shelfA0;
        shelf.a1 = 2.0 * (shelfK * shelfK - 1.0) / shelfA0;
        shelf.a2 = (1.0 - shelfK / shelfQ + shelfK * shelfK) / shelfA0;

        auto& highPass = channel[1];
        const auto highK = std::tan (pi * 38.13547087602444 / rate);
        const auto highQ = 0.5003270373238773;
        const auto highA0 = 1.0 + highK / highQ + highK * highK;
        highPass.b0 = 1.0;
        highPass.b1 = -2.0;
        highPass.b2 = 1.0;
        highPass.a1 = 2.0 * (highK * highK - 1.0) / highA0;
        highPass.a2 = (1.0 - highK / highQ + highK * highK) / highA0;
    }

    oversampling =
        std::make_unique<juce::dsp::Oversampling<float>> (2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, false);
    oversampling->initProcessing ((size_t) maximumBlockSize);
    binPower.assign ((size_t) histogramBins, 0.0);
    binCount.assign ((size_t) histogramBins, 0);
    blockPower = 0.0;
    blockDone = blocksMeasured = 0;
    lastBlocks.fill (0.0);
    momentary.store (none, std::memory_order_relaxed);
    shortTerm.store (none, std::memory_order_relaxed);
    startAgain();
}

void LoudnessMeter::process (const float* const* channels, int numChannels, int numSamples) noexcept
{
    if (oversampling == nullptr || numChannels <= 0)
        return;

    if (resetWanted.exchange (false, std::memory_order_acquire))
        startAgain();

    // Its loudness: the K-weighted power of both channels (one counted once), a 100 ms block at a time.
    const auto sides = std::min (numChannels, 2);

    for (int i = 0; i < numSamples; ++i)
    {
        for (int side = 0; side < sides; ++side)
        {
            auto& channel = filters[(size_t) side];
            const auto weighted = channel[1].process (channel[0].process ((double) channels[side][i]));
            blockPower += weighted * weighted;
        }

        if (++blockDone == blockLength)
            finishBlock();
    }

    // Its true peak, four times oversampled, in pieces as long as it was prepared for.
    const float* pair[] = { channels[0], channels[sides - 1] };

    for (int done = 0; done < numSamples;)
    {
        const auto length = std::min (numSamples - done, maximumBlockSize);
        const juce::dsp::AudioBlock<const float> block (pair, 2, (size_t) done, (size_t) length);
        const auto upsampled = oversampling->processSamplesUp (block);

        for (size_t side = 0; side < upsampled.getNumChannels(); ++side)
        {
            const auto range =
                juce::FloatVectorOperations::findMinAndMax (upsampled.getChannelPointer (side), (int) upsampled.getNumSamples());
            heldPeak = std::max ({ heldPeak, std::abs (range.getStart()), std::abs (range.getEnd()) });
        }

        done += length;
    }

    truePeak.store (heldPeak, std::memory_order_relaxed);
}

void LoudnessMeter::finishBlock() noexcept
{
    const auto meanPower = blockPower / blockLength;
    lastBlocks[(size_t) (blocksMeasured % blocksInShortTerm)] = meanPower;
    ++blocksMeasured;
    ++blocksSinceReset;
    blockPower = 0.0;
    blockDone = 0;

    const auto meanOfLast = [this] (int count)
    {
        auto sum = 0.0;

        for (int i = 1; i <= count; ++i)
            sum += lastBlocks[(size_t) ((blocksMeasured - i) % blocksInShortTerm)];

        return sum / count;
    };

    if (blocksMeasured >= blocksInShortTerm)
        shortTerm.store ((float) toLufs (meanOfLast (blocksInShortTerm)), std::memory_order_relaxed);

    if (blocksMeasured < blocksInMomentary)
        return;

    // Each 400 ms gating block, every 100 ms (three quarters overlapping): the momentary loudness, and into the histogram
    // above the absolute gate.
    const auto gatingPower = meanOfLast (blocksInMomentary);
    const auto loudness = toLufs (gatingPower);
    momentary.store ((float) loudness, std::memory_order_relaxed);

    if (loudness <= histogramFloor || blocksSinceReset < blocksInMomentary)
        return;

    const auto bin = juce::jlimit (0, histogramBins - 1, (int) ((loudness - histogramFloor) * 10.0));
    binPower[(size_t) bin] += gatingPower;
    ++binCount[(size_t) bin];

    // The integrated loudness: the blocks above the absolute gate, then those no more than 10 LU below their loudness.
    auto power = 0.0;
    auto count = 0;

    for (int i = 0; i < histogramBins; ++i)
    {
        power += binPower[(size_t) i];
        count += binCount[(size_t) i];
    }

    const auto relativeGate = toLufs (power / count) - 10.0;
    const auto first = juce::jlimit (0, histogramBins, (int) std::ceil ((relativeGate - histogramFloor) * 10.0));
    power = 0.0;
    count = 0;

    for (int i = first; i < histogramBins; ++i)
    {
        power += binPower[(size_t) i];
        count += binCount[(size_t) i];
    }

    integrated.store (count > 0 ? (float) toLufs (power / count) : none, std::memory_order_relaxed);
}

void LoudnessMeter::startAgain() noexcept
{
    std::fill (binPower.begin(), binPower.end(), 0.0);
    std::fill (binCount.begin(), binCount.end(), 0);
    heldPeak = 0.0f;
    blocksSinceReset = 0;
    blockPower = 0.0; // a block from now, none of it heard before (the filters run on, as a reset mustn't ring)
    blockDone = 0;
    integrated.store (none, std::memory_order_relaxed);
    truePeak.store (0.0f, std::memory_order_relaxed);
}

LoudnessMeter::Readings LoudnessMeter::getReadings() const noexcept
{
    const auto read = [] (const std::atomic<float>& value) -> std::optional<float>
    {
        const auto loudness = value.load (std::memory_order_relaxed);
        return loudness > none ? std::optional (loudness) : std::nullopt;
    };

    const auto peak = truePeak.load (std::memory_order_relaxed);
    return { read (momentary), read (shortTerm), read (integrated),
             peak > 1.0e-5f ? std::optional (juce::Decibels::gainToDecibels (peak)) : std::nullopt }; // above -100 dB
}
} // namespace phazite::engine
