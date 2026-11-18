// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TapeWear.h"
#include "devices/dsp/Math.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
namespace
{
    constexpr std::uint64_t dropoutSeed = 0x746170652d776561ull; // where the tape is worn, the same on every tape
    constexpr std::uint32_t hissSeeds[] = { 1963u, 4711u };
    constexpr double fadeSeconds = 0.02, glideSeconds = 0.02;
    constexpr double butterworthQ = 0.70710678;
    constexpr double widestDipTop = 20000.0, narrowestDipTop = 3000.0; // where the dip's low-pass is, at no dip and at 18 dB
    constexpr double fewestASecond = 1.0 / 8.0, mostASecond = 2.0;     // dropouts a second, at 10 % and at 100 %
    constexpr float leastDepth = 3.0f;                                 // decibels, at 10 %
    constexpr float firstStep = 0.1f;                                  // the amount the figures above start from
    constexpr int longestRun = 64; // steps drawn one by one after the last; more is a jump, drawn again from the steps before

    // SplitMix64's finalizer, as SurfaceNoise's: a step of the song's time, mixed into bits as good as random.
    std::uint64_t mix (std::uint64_t x) noexcept
    {
        x += 0x9e3779b97f4a7c15ull;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
        return x ^ (x >> 31);
    }

    // In [0, 1).
    double toUnit (std::uint64_t bits) noexcept
    {
        return (double) (bits >> 11) * (1.0 / 9007199254740992.0);
    }
} // namespace

double TapeWear::getDipsPerSecond (float amount) noexcept
{
    if (amount <= 0.0f)
        return 0.0;

    return fewestASecond * std::pow (mostASecond / fewestASecond, ((double) amount - firstStep) / (1.0 - firstStep));
}

float TapeWear::getDipDepth (float amount) noexcept
{
    if (amount <= 0.0f)
        return 0.0f;

    if (amount < firstStep)
        return leastDepth * amount / firstStep;

    return leastDepth + (deepestDip - leastDepth) * std::min (1.0f, (amount - firstStep) / (1.0f - firstStep));
}

void TapeWear::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    fadeStep = (float) (1.0 / (fadeSeconds * sampleRate));
    smoothing = (float) (1.0 - std::exp (-1.0 / (glideSeconds * sampleRate)));
    hissLow = dsp::makeFilter (dsp::FilterShape::highPass, hissLowCut, sampleRate, butterworthQ);
    hissTop = dsp::makeFilter (dsp::FilterShape::highShelf, hissShelf, sampleRate, butterworthQ, hissShelfGain);

    for (auto& noise : hissNoise)
        noise.prepare (sampleRate);

    // What makes the hiss's RMS 1 at 48 kHz, the rate Phazite's presets are leveled at: even noise from -1 to 1 has a variance
    // of a third, which the filters as made at 48 kHz multiply by the energy of their answer to an impulse (a second of it, by
    // when the high-pass has long died away). The same scale at every rate: the noise (dsp::Noise) is as loud for each hertz
    // at every rate, so the hiss is too, the band above 24 kHz of a higher rate holding none of it, and 0.37 dB less at 44.1 kHz.
    constexpr double levelRate = 48000.0;
    const auto low48 = dsp::makeFilter (dsp::FilterShape::highPass, hissLowCut, levelRate, butterworthQ);
    const auto top48 = dsp::makeFilter (dsp::FilterShape::highShelf, hissShelf, levelRate, butterworthQ, hissShelfGain);
    dsp::Filter low, top;
    auto energy = 0.0;

    for (int i = 0; i < (int) levelRate; ++i)
    {
        const auto out = (double) top.process (low.process (i == 0 ? 1.0f : 0.0f, low48), top48);
        energy += out * out;
    }

    hissScale = (float) (1.0 / std::sqrt (energy / 3.0));
    reset();
}

void TapeWear::reset() noexcept
{
    for (size_t i = 0; i < 2; ++i)
    {
        hissNoise[i].setSeed (hissSeeds[i]);
        hissLowFilters[i].reset();
        hissTopFilters[i].reset();
        dipFilters[i].reset();
    }

    hissShare = hissNow = 0.0f;
    dipShare = depthNow = lastDip = 0.0f;
    dipFiltering = false;

    for (auto& dip : dips)
        dip.on = false;

    hasPosition = hasStep = false;
    position = beatsPerSample = 0.0;
    dipNow.set (0.0f);
}

void TapeWear::jumpTo (bool hissWanted, bool dipsWanted) noexcept
{
    hissShare = hissWanted ? 1.0f : 0.0f;
    hissNow = hissGain;
    dipShare = dipsWanted ? 1.0f : 0.0f;
    depthNow = getDipDepth (dropouts);
}

void TapeWear::setPosition (double beat, double newBeatsPerSample, double offset) noexcept
{
    if (! hasPosition || ! juce::exactlyEqual (beat, givenBeat))
        position = beat + offset;

    givenBeat = beat;
    beatsPerSample = newBeatsPerSample;
    hasPosition = true;
}

void TapeWear::drawSteps() noexcept
{
    const auto step = (std::int64_t) std::floor (position * stepsPerBeat);

    if (hasStep && step == lastStep)
        return;

    if (! hasStep || step < lastStep || step - lastStep > longestRun)
    {
        // A jump (or the first step): the dips that started in the last 250 ms and are under way here, drawn again.
        for (auto& dip : dips)
            dip.on = false;

        const auto beatsPerSecond = beatsPerSample * sampleRate;
        const auto back = std::min ((std::int64_t) 8192, (std::int64_t) std::ceil (longestDip * beatsPerSecond * stepsPerBeat));

        for (auto each = step - back; each <= step; ++each)
            drawStep (each, false);
    }
    else
    {
        for (auto each = lastStep + 1; each <= step; ++each)
            drawStep (each, true);
    }

    lastStep = step;
    hasStep = true;
}

void TapeWear::drawStep (std::int64_t step, bool counts) noexcept
{
    // The step's chance of a dropout: so many a second, over the steps a second at the song's tempo now.
    auto bits = mix (dropoutSeed ^ (std::uint64_t) step);
    const auto beatsPerSecond = beatsPerSample * sampleRate;
    const auto stepsPerSecond = stepsPerBeat * beatsPerSecond;

    if (toUnit (bits) * stepsPerSecond >= getDipsPerSecond (dropouts))
        return;

    bits = mix (bits);
    const auto seconds = shortestDip + (longestDip - shortestDip) * toUnit (bits);
    bits = mix (bits);
    const auto depth = (float) (0.5 + 0.5 * toUnit (bits));
    const auto start = (double) step / stepsPerBeat, length = seconds * beatsPerSecond;

    // A free place, or the one that started first.
    auto* dip = &dips[0];

    for (auto& each : dips)
    {
        if (! each.on || each.start + each.length <= start)
        {
            dip = &each;
            break;
        }

        if (each.start < dip->start)
            dip = &each;
    }

    dip->on = true;
    dip->start = start;
    dip->length = length;
    dip->depth = depth;

    if (counts)
        dipsMade.fetch_add (1, std::memory_order_relaxed);
}

TapeWear::Shape TapeWear::getShape() const noexcept
{
    Shape deepest;

    for (const auto& dip : dips)
    {
        if (! dip.on)
            continue;

        const auto along = (position - dip.start) / dip.length;

        if (along > 0.0 && along < 1.0)
        {
            const auto shape = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * along));

            if (shape * dip.depth > deepest.depth)
                deepest = { shape * dip.depth, shape };
        }
    }

    return deepest;
}

void TapeWear::process (float& left, float& right, bool hissWanted, bool dipsWanted) noexcept
{
    // The dropouts: the sound dipped, its top closing, where one is under way.
    dipShare = dipsWanted ? std::min (1.0f, dipShare + fadeStep) : std::max (0.0f, dipShare - fadeStep);
    lastDip = 0.0f;

    if (dipShare > 0.0f)
    {
        const auto targetDepth = getDipDepth (dropouts);
        depthNow += (targetDepth - depthNow) * smoothing;

        if (std::abs (targetDepth - depthNow) < 1.0e-5f)
            depthNow = targetDepth;

        if (beatsPerSample > 0.0 && dropouts > 0.0f)
            drawSteps();

        const auto deepest = getShape();
        const auto shape = deepest.along * dipShare; // how far the top's low-pass is in

        if (deepest.depth > 0.0f && depthNow > 0.0f)
        {
            lastDip = depthNow * deepest.depth * dipShare;
            const auto gain = dsp::decibelsToGain (-lastDip);
            const auto top =
                std::min (widestDipTop * std::pow (narrowestDipTop / widestDipTop, (double) lastDip / deepestDip), 0.45 * sampleRate);
            const auto lowPass = dsp::makeFilter (dsp::FilterShape::lowPass, top, sampleRate, butterworthQ);
            const auto l = dipFilters[0].process (left, lowPass), r = dipFilters[1].process (right, lowPass);
            left = gain * (left + shape * (l - left));
            right = gain * (right + shape * (r - right));
            dipFiltering = true;
        }
        else if (dipFiltering)
        {
            // Between dips, the low-pass rests: the next starts it from nothing, under a dip of next to nothing.
            dipFilters[0].reset();
            dipFilters[1].reset();
            dipFiltering = false;
        }
    }
    else if (dipFiltering)
    {
        dipFilters[0].reset();
        dipFilters[1].reset();
        dipFiltering = false;
    }

    position += beatsPerSample;

    // The hiss, each side its own.
    hissShare = hissWanted ? std::min (1.0f, hissShare + fadeStep) : std::max (0.0f, hissShare - fadeStep);

    if (hissShare > 0.0f)
    {
        hissNow += (hissGain - hissNow) * smoothing;

        if (hissGain <= 0.0f && hissNow < 1.0e-9f)
            hissNow = 0.0f;

        if (hissNow > 0.0f)
        {
            const auto gain = hissShare * hissNow * hissScale;
            left += gain * hissTopFilters[0].process (hissLowFilters[0].process (hissNoise[0].next(), hissLow), hissTop);
            right += gain * hissTopFilters[1].process (hissLowFilters[1].process (hissNoise[1].next(), hissLow), hissTop);
        }
    }
}
} // namespace phazite::devices
