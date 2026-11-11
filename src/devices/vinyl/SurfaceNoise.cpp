// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SurfaceNoise.h"

#include <juce_core/juce_core.h>

namespace phazite::devices
{
namespace
{
    constexpr std::uint64_t crackleSeed = 0x76696e796c2d3333ull; // the dust, the same on every record
    constexpr std::uint32_t hissSeeds[] = { 1980u, 3333u, 7845u };
    constexpr double fadeSeconds = 0.02;      // the share's glide, from nothing to all or back
    constexpr double hissGlideSeconds = 0.02; // a new hiss level's, about
    constexpr double narrowest = 15.0e-6;     // the smallest click's width (its Gaussian's sigma), in seconds
    constexpr double widening = 7.0;          // the largest is this much more again: a pop
    constexpr float lean = 0.3f;              // how far a click leans to one side, at most
    constexpr double hissPoles[] = { 60.0, 480.0 }, hissZeros[] = { 170.0, 1357.0 }; // an octave and a half apart, in hertz

    // SplitMix64's finalizer: a step of the song's time, mixed into bits that are as good as random, the same every time.
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

void SurfaceNoise::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    fadeStep = (float) (1.0 / (fadeSeconds * sampleRate));
    hissSmoothing = (float) (1.0 - std::exp (-1.0 / (hissGlideSeconds * sampleRate)));

    // The hiss's shape at this rate, and what makes its RMS 1: even noise from -1 to 1 has a variance of a third, which the
    // shape multiplies by the energy of its answer to an impulse (two seconds of it, by when the lowest pole has died away).
    const auto toPole = [this] (double hertz) { return (float) std::exp (-juce::MathConstants<double>::twoPi * hertz / sampleRate); };

    for (size_t i = 0; i < 2; ++i)
    {
        hissShape.poles[i] = toPole (hissPoles[i]);
        hissShape.zeros[i] = toPole (hissZeros[i]);
    }

    hissShape.poles[2] = toPole (hissTop);
    hissShape.topGain = 1.0f - hissShape.poles[2];
    Hiss impulse;
    auto energy = 0.0;

    for (int i = 0; i < (int) (2.0 * sampleRate); ++i)
    {
        const auto out = (double) impulse.process (i == 0 ? 1.0f : 0.0f, hissShape);
        energy += out * out;
    }

    hissScale = (float) (1.0 / std::sqrt (energy / 3.0));
    reset();
}

void SurfaceNoise::reset() noexcept
{
    commonNoise.setSeed (hissSeeds[0]);
    ownNoise[0].setSeed (hissSeeds[1]);
    ownNoise[1].setSeed (hissSeeds[2]);
    hiss = {};
    hissNow = 0.0f;
    share = 0.0f;

    for (auto& click : clicks)
        click.on = false;

    activeClicks = 0;
    hasPosition = hasStep = false;
    position = beatsPerSample = 0.0;
}

void SurfaceNoise::setPosition (double beat, double newBeatsPerSample, double offset) noexcept
{
    if (! hasPosition || ! juce::exactlyEqual (beat, givenBeat))
        position = beat + offset;

    givenBeat = beat;
    beatsPerSample = newBeatsPerSample;
    hasPosition = true;
}

void SurfaceNoise::drawSteps() noexcept
{
    const auto step = (std::int64_t) std::floor (position * stepsPerBeat);

    if (hasStep && step == lastStep)
        return;

    // The steps passed since the last sample, normally one at most; after a jump, only the one it landed on.
    if (! hasStep || step < lastStep || step - lastStep > 4)
        drawStep (step);
    else
        for (auto each = lastStep + 1; each <= step; ++each)
            drawStep (each);

    lastStep = step;
    hasStep = true;
}

void SurfaceNoise::drawStep (std::int64_t step) noexcept
{
    // The step's chance of a click: so many a second, over the steps a second at the song's tempo now.
    auto bits = mix (crackleSeed ^ (std::uint64_t) step);
    const auto stepsPerSecond = stepsPerBeat * beatsPerSample * sampleRate;

    if (toUnit (bits) * stepsPerSecond >= (double) density)
        return;

    bits = mix (bits);
    const auto chance = 1.0 - toUnit (bits); // (0, 1]
    const auto size = (float) (chance * chance * chance * chance);
    bits = mix (bits);
    const auto side = (float) toUnit (bits);
    const auto polarity = (bits & 1u) != 0 ? 1.0f : -1.0f;

    // A free click, or the one nearest its end.
    auto* click = &clicks[0];

    for (auto& each : clicks)
    {
        if (! each.on)
        {
            click = &each;
            break;
        }

        if (each.at / each.length > click->at / click->length)
            click = &each;
    }

    if (! click->on)
        ++activeClicks;

    click->on = true;
    click->at = 0.0f;
    click->width = (float) std::max (0.7, narrowest * (1.0 + widening * size) * sampleRate);
    click->length = 8.0f * click->width;
    const auto peak = polarity * crackleGain * share * size;
    const auto towards = (2.0f * side - 1.0f) * lean;
    click->left = peak * (1.0f - towards);
    click->right = peak * (1.0f + towards);

    clicksMade.fetch_add (1, std::memory_order_relaxed);
    largest.raise (size);
}

void SurfaceNoise::playClicks (float& left, float& right) noexcept
{
    for (auto& click : clicks)
    {
        if (! click.on)
            continue;

        // A Gaussian's slope, its peaks (at one width either side of its middle) 1.
        const auto t = (click.at - 0.5f * click.length) / click.width;
        const auto slope = -t * std::exp (0.5f * (1.0f - t * t));
        left += click.left * slope;
        right += click.right * slope;

        if ((click.at += 1.0f) >= click.length)
        {
            click.on = false;
            --activeClicks;
        }
    }
}
} // namespace phazite::devices
