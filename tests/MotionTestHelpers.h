// SPDX-License-Identifier: AGPL-3.0-or-later
// What the motion effects' tests share (MotionEffectTests.cpp, ChorusTests.cpp): an effect played through its runner, as
// Phazite's engine plays it (engine/nodes/DeviceRunner.h), with what came in mixed back in as its mix says; and a tone read
// in what comes out: the sine at its frequency that fits each side best, the side's (half the left less the right), and
// what of the tone isn't steady, which a sweep moves.
#pragma once

#include "TestHelpers.h"
#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"
#include "engine/nodes/DeviceRunner.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <memory>
#include <vector>

namespace phazite::test
{
// A device's values, each parameter at its default.
inline std::vector<float> getDefaultValues (const devices::DeviceInfo& info)
{
    std::vector<float> values;

    for (const auto& parameter : info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

// A sine so many samples long.
inline std::vector<float> makeTone (double hertz, int numSamples, double sampleRate, float amplitude = 0.5f)
{
    std::vector<float> sound ((size_t) numSamples);

    for (int i = 0; i < numSamples; ++i)
        sound[(size_t) i] = amplitude * (float) std::sin (juce::MathConstants<double>::twoPi * hertz * i / sampleRate);

    return sound;
}

// Plays a sound, the same on both sides, through a device's runner in blocks of a size, the song stopped. Before each block,
// `beforeBlock` is called with its number and may change the values, which the runner then glides to over the block, as it
// does a knob's.
inline juce::AudioBuffer<float> playThroughRunner (const devices::DeviceInfo& info, std::unique_ptr<devices::Processor> processor,
                                                   std::vector<float> values, const std::vector<float>& sound, double sampleRate,
                                                   int blockSize, const std::function<void (int, std::vector<float>&)>& beforeBlock = {})
{
    engine::DeviceRunner runner (info, std::move (processor));
    runner.prepare (sampleRate, blockSize);
    const auto numSamples = (int) sound.size();
    juce::AudioBuffer<float> out (2, numSamples);

    for (int channel = 0; channel < 2; ++channel)
        std::copy (sound.begin(), sound.end(), out.getWritePointer (channel));

    for (int start = 0, number = 0; start < numSamples; start += blockSize, ++number)
    {
        if (beforeBlock)
            beforeBlock (number, values);

        engine::DeviceRunner::Block block;
        block.values = values;
        runner.process (out, start, std::min (blockSize, numSamples - start), block);
    }

    return out;
}

// A tone in a stereo sound, from a sample over as many whole cycles of it as fit before another: the amplitude of the sine
// at its frequency that fits each side best; and, each as an amplitude (its RMS times the square root of two), the side
// (half the left less the right, all of it), and the larger on the two sides of what is left once that sine is taken away:
// what moves of the tone.
struct ToneParts
{
    double left = 0.0, right = 0.0, side = 0.0, unsteady = 0.0;
};

inline ToneParts readTone (const juce::AudioBuffer<float>& sound, double hertz, double sampleRate, int from, int to)
{
    constexpr auto twoPi = juce::MathConstants<double>::twoPi;
    const auto length = std::max (1, (int) std::lround (std::floor ((to - from) * hertz / sampleRate) * sampleRate / hertz));
    ToneParts parts;
    auto sideEnergy = 0.0;

    for (int i = from; i < from + length; ++i)
    {
        const auto side = 0.5 * (sound.getSample (0, i) - sound.getSample (1, i));
        sideEnergy += side * side;
    }

    parts.side = std::sqrt (2.0 * sideEnergy / length);

    for (int channel = 0; channel < 2; ++channel)
    {
        std::complex<double> sine;

        for (int i = from; i < from + length; ++i)
            sine += (double) sound.getSample (channel, i) * std::polar (1.0, -twoPi * hertz * i / sampleRate);

        sine *= 2.0 / length;
        (channel == 0 ? parts.left : parts.right) = std::abs (sine);
        auto energy = 0.0;

        for (int i = from; i < from + length; ++i)
        {
            const auto rest = (double) sound.getSample (channel, i) - (sine * std::polar (1.0, twoPi * hertz * i / sampleRate)).real();
            energy += rest * rest;
        }

        parts.unsteady = std::max (parts.unsteady, std::sqrt (2.0 * energy / length));
    }

    return parts;
}

// A gain in decibels.
inline double inDecibels (double gain)
{
    return 20.0 * std::log10 (std::max (gain, 1.0e-12));
}
} // namespace phazite::test
