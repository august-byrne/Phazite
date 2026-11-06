// SPDX-License-Identifier: AGPL-3.0-or-later
#include "StartupSound.h"
#include "Preview.h"
#include "devices/delay/Delay.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace phazite
{
namespace
{
    // The blip: six quick steps that zig-zag, a high line rising by fifths between a low line rising by whole tones, high
    // first, each about a sixtieth of a second (the high ones shortening, and quieter as it goes). Each step swells from
    // nothing to its middle and back (a raised cosine), so that the steps join in silence without clicks, and is a tone with
    // two partials that ring out faster than it: its third, a little sharp, as loud as the tone at first, and a metallic
    // one at 5.4 times its pitch, 10 dB over the tone at first.
    struct Step
    {
        double hz, seconds, decibels;
    };

    constexpr std::array<Step, 6> steps { { { 990.0, 0.019, 0.0 },
                                            { 663.0, 0.019, -0.5 },
                                            { 1485.0, 0.015, -3.3 },
                                            { 745.0, 0.019, -2.5 },
                                            { 2221.0, 0.014, -10.7 },
                                            { 834.0, 0.019, -6.7 } } };
    constexpr double thirdRatio = 3.03, thirdLevel = 1.05, thirdSeconds = 0.015;
    constexpr double ringRatio = 5.4, ringLevel = 3.3, ringSeconds = 0.009;

    // Then its echoes from Phazite's delay, bouncing between the sides every 44 ms, the first about 16 dB under the blip on
    // its side, each after it 2 dB under the one before and as bright: a quick shimmer that rings on, without a room. The
    // whole sound fades out over its last part, wherever the echoes are by then, and is brought to its peak.
    constexpr float echoMilliseconds = 44.0f, echoFeedback = 0.81f, echoLowCutHz = 20.0f, echoHighCutHz = 20000.0f;
    constexpr float echoLevel = 0.16f;
    constexpr double lengthSeconds = 1.5, fadeOutSeconds = 0.3;
    constexpr float peak = 0.5f; // -6 dBFS
    constexpr int blockSize = 512;

    // Plays a stereo sound through one of Phazite's devices, a block at a time as Phazite's engine plays it, in place.
    template <typename Device>
    void process (Device& device, juce::AudioBuffer<float>& sound, std::span<const float> parameters, double sampleRate)
    {
        device.prepare (sampleRate, blockSize);

        for (int start = 0; start < sound.getNumSamples(); start += blockSize)
        {
            float* channels[] = { sound.getWritePointer (0, start), sound.getWritePointer (1, start) };
            devices::ProcessContext block;
            block.channels = channels;
            block.numChannels = 2;
            block.numSamples = std::min (blockSize, sound.getNumSamples() - start);
            block.parameters = parameters;
            block.isRendering = true;
            device.process (block);
        }
    }
} // namespace

juce::AudioBuffer<float> makeStartupSound (double sampleRate)
{
    const auto length = (int) std::ceil (lengthSeconds * sampleRate);
    juce::AudioBuffer<float> sound (2, std::max (0, length));
    sound.clear();

    if (length <= 0)
        return sound;

    // The blip, the same on both sides: each step a tone of its own, so that none changes pitch (which would click).
    auto* blip = sound.getWritePointer (0);
    auto blipLength = 0;
    auto stepStart = 0.0;

    for (const auto& step : steps)
    {
        const auto start = (int) std::lround (stepStart * sampleRate);
        const auto count = std::min (length - start, (int) std::lround (step.seconds * sampleRate));
        const auto level = juce::Decibels::decibelsToGain (step.decibels);
        const auto radiansPerSecond = juce::MathConstants<double>::twoPi * step.hz;

        for (int i = 0; i < count; ++i)
        {
            const auto t = i / sampleRate;
            const auto swell = std::sin (juce::MathConstants<double>::pi * t / step.seconds);
            const auto envelope = level * swell * swell;
            const auto phase = radiansPerSecond * t;
            const auto tone = std::sin (phase) + thirdLevel * std::exp (-t / thirdSeconds) * std::sin (thirdRatio * phase)
                              + ringLevel * std::exp (-t / ringSeconds) * std::sin (ringRatio * phase);
            blip[start + i] += (float) (envelope * tone);
        }

        blipLength = std::max (blipLength, start + count);
        stepStart += step.seconds;
    }

    sound.copyFrom (1, 0, sound, 0, 0, blipLength);

    // Its echoes, bouncing between the sides: the delay puts out the echoes alone.
    juce::AudioBuffer<float> echoes (sound);
    std::array<float, devices::Delay::mix + 1> echo {};
    echo[devices::Delay::sync] = 0.0f;
    echo[devices::Delay::milliseconds] = echoMilliseconds;
    echo[devices::Delay::feedback] = echoFeedback;
    echo[devices::Delay::pingPong] = 1.0f;
    echo[devices::Delay::lowCut] = echoLowCutHz;
    echo[devices::Delay::highCut] = echoHighCutHz;
    echo[devices::Delay::mix] = 1.0f;
    devices::Delay delay;
    process (delay, echoes, echo, sampleRate);

    const auto fadeLength = std::min (length, (int) std::ceil (fadeOutSeconds * sampleRate));

    for (int channel = 0; channel < 2; ++channel)
    {
        sound.addFrom (channel, 0, echoes, channel, 0, length, echoLevel);
        sound.applyGainRamp (channel, length - fadeLength, fadeLength, 1.0f, 0.0f);
    }

    if (const auto loudest = sound.getMagnitude (0, length); loudest > 0.0f)
        sound.applyGain (peak / loudest);

    return sound;
}

void playStartupSound (Previewer& previewer)
{
    const auto rate = previewer.getSampleRate();
    previewer.playSound (makeStartupSound (rate), rate);
}
} // namespace phazite
