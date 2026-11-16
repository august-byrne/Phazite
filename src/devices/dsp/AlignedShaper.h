// SPDX-License-Identifier: AGPL-3.0-or-later
// A curve at twice the rate (AntialiasedShaper, Oversampling.h) whose delay is a whole number of samples, so that a plain
// delay as long lines up with it: its half-band filters and its curve delay the lows 5.137 samples, and a first-order
// all-pass after them adds the rest of a sample, as juce::dsp::Oversampling makes up its own fractional delay with one. A
// device that fades between the curve and a plain delay (the compressor's drive, the multiband's saturation), or reports
// its latency in whole samples, plays the two in step: the curve's sound came 0.137 samples later than the latency
// reported. Real-time safe.
#pragma once

#include "Oversampling.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>

namespace phazite::devices::dsp
{
// A delay of a part of a sample: a first-order all-pass, its delay at 0 Hz exactly the part asked for, as flat as such a
// filter's can be there (Thiran, 1971); for a part near one, as here, within a twentieth of a sample of it to a quarter of
// the rate. From half a sample to one and a half: its coefficient within a third of 0 either way, so it is stable and its
// delay keeps flat (at a part under 0 it would run away). Real-time safe.
class FractionalDelay
{
public:
    static constexpr float shortest = 0.5f, longest = 1.5f; // the parts it is made for

    void setDelay (float samples) noexcept
    {
        jassert (samples >= shortest && samples < longest);
        samples = std::clamp (samples, shortest, longest);
        coefficient = (1.0f - samples) / (1.0f + samples);
    }

    void reset() noexcept { lastIn = lastOut = 0.0f; }

    float process (float x) noexcept
    {
        const auto y = coefficient * (x - lastOut) + lastIn;
        lastIn = x;
        lastOut = y;
        return y;
    }

private:
    float coefficient = 0.0f, lastIn = 0.0f, lastOut = 0.0f;
};

class AlignedShaper
{
public:
    AlignedShaper() { rest.setDelay ((float) getLatencySamples() - AntialiasedShaper::getLatencySamples()); }

    // How late it plays what it is given, in samples, at low frequencies: its shaper's delay, made up to a whole number of
    // samples by half a sample to one and a half (FractionalDelay's).
    static int getLatencySamples()
    {
        static const int latency = getWholeSamples (AntialiasedShaper::getLatencySamples());
        return latency;
    }

    // The whole number of samples a delay is made up to: more than it by half a sample to one and a half.
    static int getWholeSamples (float samples) noexcept { return (int) std::ceil (samples + FractionalDelay::shortest); }

    void reset() noexcept
    {
        shaper.reset();
        rest.reset();
    }

    template <typename Curve>
    float process (float sample, const Curve& curve) noexcept
    {
        return rest.process (shaper.process (sample, curve));
    }

    // After a change of curve, its averaging starts again.
    void restartAveraging() noexcept { shaper.restartAveraging(); }

private:
    AntialiasedShaper shaper;
    FractionalDelay rest;
};
} // namespace phazite::devices::dsp
