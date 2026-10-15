// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Reverb.h"
#include "devices/dsp/Oscillators.h"
#include "devices/dsp/Random.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
namespace
{
    // The IDs are saved in projects: never change them.
    constexpr ParameterInfo parameters[] = {
        { .id = "size", .name = "Size", .defaultValue = 0.5f, .unit = Unit::percent },
        { .id = "decay",
          .name = "Decay",
          .minimum = 0.2f,
          .maximum = 20.0f,
          .defaultValue = 2.0f,
          .unit = Unit::seconds,
          .midpoint = 2.0f },
        { .id = "predelay",
          .name = "Pre-Delay",
          .minimum = 0.0f,
          .maximum = 250.0f,
          .defaultValue = 10.0f,
          .unit = Unit::milliseconds,
          .midpoint = 40.0f },
        { .id = "damping",
          .name = "Damping",
          .minimum = 1000.0f,
          .maximum = 20000.0f,
          .defaultValue = 6000.0f,
          .unit = Unit::hertz,
          .midpoint = 4472.0f }, // the middle of the range, on a log scale
        { .id = "lowCut",
          .name = "Low Cut",
          .minimum = 20.0f,
          .maximum = 1000.0f,
          .defaultValue = 100.0f,
          .unit = Unit::hertz,
          .midpoint = 141.0f },
        { .id = "width", .name = "Width", .defaultValue = 1.0f, .unit = Unit::percent },
        { .id = mixParameterID, .name = "Mix", .defaultValue = 0.25f, .unit = Unit::percent },
    };

    // In a mixer slot: how long it rings and how big it is.
    constexpr std::string_view keyControls[] = { "decay", "size" };

    // The lines' lengths at the largest size, in milliseconds: spread apart, and sharing no common period.
    constexpr double lineMilliseconds[Reverb::numLines] = { 44.3, 49.8, 57.1, 63.4, 69.7, 74.3, 82.9, 89.5 };
    constexpr double smallest = 0.25; // of those lengths, at size 0

    // How far each line's length moves, and how fast: slowly, each at a rate of its own.
    constexpr double modulationMilliseconds = 0.12;
    constexpr double modulationHertz[Reverb::numLines] = { 0.31, 0.47, 0.53, 0.67, 0.79, 0.89, 1.01, 1.13 };
    constexpr std::uint32_t modulationSeed = 0x5eed1234u;

    // The allpass filters before the network, from Dattorro's plate (1997): their lengths in milliseconds, and gains.
    constexpr double diffuserMilliseconds[] = { 4.77, 3.59, 12.73, 9.30 };
    constexpr float diffuserGains[] = { 0.75f, 0.75f, 0.625f, 0.625f };
    constexpr double rightSpread = 1.07; // the right channel's are a little longer, so the sides differ

    // Which lines each side hears, and how: the two patterns are orthogonal, so the sides are uncorrelated.
    constexpr float leftSigns[Reverb::numLines] = { 1, -1, 1, -1, 1, -1, 1, -1 };
    constexpr float rightSigns[Reverb::numLines] = { 1, 1, -1, -1, 1, 1, -1, -1 };
    constexpr float lineScale = 0.35355339f;  // 1 / sqrt (numLines): how much of each side goes into each of its lines
    constexpr float outputGain = 0.70710678f; // so that a 2 s tail of steady noise sits about 6 dB under the noise

    constexpr double smoothingSeconds = 0.1; // how long the size and pre-delay take to follow their knobs, about

    double getLineSamples (int line, float size, double sampleRate)
    {
        return lineMilliseconds[line] * 0.001 * sampleRate * (smallest + (1.0 - smallest) * size);
    }
} // namespace

const DeviceInfo Reverb::info { .typeID = typeID,
                                .name = "Reverb",
                                .kind = DeviceKind::effect,
                                .parameters = parameters,
                                .keyControls = keyControls };

double Reverb::getDecayAt (std::span<const float> values, double frequency, double sampleRate)
{
    // A pass through an average line: the loss that makes the decay, and the damping filter's at this frequency.
    auto meanSamples = 0.0;

    for (int line = 0; line < numLines; ++line)
        meanSamples += getLineSamples (line, values[size], sampleRate) / numLines;

    const auto decibelsPerPass = 60.0 * meanSamples / ((double) values[decay] * sampleRate);
    const auto c = 1.0 - std::exp (-juce::MathConstants<double>::twoPi * values[damping] / sampleRate);
    const auto omega = juce::MathConstants<double>::twoPi * std::min (frequency, 0.5 * sampleRate) / sampleRate;
    const auto magnitude = c / std::sqrt (1.0 - 2.0 * (1.0 - c) * std::cos (omega) + (1.0 - c) * (1.0 - c));
    const auto damped = -20.0 * std::log10 (magnitude);
    return 60.0 * meanSamples / (sampleRate * (decibelsPerPass + damped));
}

void Reverb::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    smoothing = (float) (1.0 - std::exp (-1.0 / (smoothingSeconds * sampleRate)));
    const auto modulationDepth = modulationMilliseconds * 0.001 * sampleRate;

    for (int line = 0; line < numLines; ++line)
    {
        lines[(size_t) line].prepare ((int) std::ceil (getLineSamples (line, 1.0f, sampleRate) + 2.0 * modulationDepth) + 2);
        phaseSteps[(size_t) line] = (float) (modulationHertz[line] / sampleRate);
    }

    for (auto& line : predelays)
        line.prepare ((int) std::ceil (0.001 * parameters[predelay].maximum * sampleRate) + 2);

    for (size_t side = 0; side < 2; ++side)
    {
        for (size_t stage = 0; stage < (size_t) numDiffusers; ++stage)
        {
            auto& allpass = diffusers[side][stage];
            allpass.length =
                std::max (1, (int) std::lround (diffuserMilliseconds[stage] * 0.001 * sampleRate * (side == 0 ? 1.0 : rightSpread)));
            allpass.gain = diffuserGains[stage];
            allpass.line.prepare (allpass.length);
        }
    }

    reset();
}

void Reverb::reset()
{
    for (auto& line : lines)
        line.reset();

    for (auto& line : predelays)
        line.reset();

    for (auto& side : diffusers)
        for (auto& allpass : side)
            allpass.line.reset();

    for (auto& filter : lowCuts)
        filter.reset();

    damped.fill (0.0f);

    // The same starting points every time, so that a render comes out the same.
    dsp::Random random (modulationSeed);

    for (auto& phase : phases)
        phase = random.nextUnipolar();

    shownSize = -1.0f;
}

double Reverb::getTailSeconds() const
{
    return (double) tailSeconds.load (std::memory_order_relaxed);
}

void Reverb::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);

    if (numChannels == 0)
        return;

    const auto decaySeconds = values[decay];
    const auto targetPredelay = (float) (values[predelay] * 0.001 * sampleRate);
    const auto dampingCoefficient = (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi * values[damping] / sampleRate));
    const auto cut = dsp::makeFilter (dsp::FilterShape::highPass, values[lowCut], sampleRate, 0.70710678);
    const auto modulationDepth = (float) (modulationMilliseconds * 0.001 * sampleRate);
    const auto spread = values[width];
    tailSeconds.store (decaySeconds + values[predelay] * 0.001f, std::memory_order_relaxed);

    if (shownSize < 0.0f)
    {
        shownSize = values[size];
        shownPredelay = targetPredelay;
    }

    // Each line's loss on each pass, so that every line fades by 60 dB over the decay time; worked out for the size at
    // the block's start.
    std::array<float, numLines> lengths {}, losses {};

    for (int line = 0; line < numLines; ++line)
    {
        lengths[(size_t) line] = (float) getLineSamples (line, shownSize, sampleRate);
        losses[(size_t) line] = (float) std::pow (10.0, -3.0 * lengths[(size_t) line] / (decaySeconds * sampleRate));
    }

    const auto sizeStep = (values[size] - shownSize) * std::min (1.0f, smoothing * (float) block.numSamples);
    const auto lengthScale = [] (float shown) { return (float) (smallest + (1.0 - smallest) * shown); };
    const auto startScale = lengthScale (shownSize), endScale = lengthScale (shownSize + sizeStep);
    shownSize += sizeStep;

    for (int i = 0; i < block.numSamples; ++i)
    {
        // In: delayed, then smeared, each side on its own.
        shownPredelay += (targetPredelay - shownPredelay) * smoothing;
        std::array<float, 2> in {};

        for (size_t side = 0; side < 2; ++side)
        {
            const auto sample = block.channels[std::min ((int) side, numChannels - 1)][i];
            auto& line = predelays[side];
            line.push (sample);
            auto diffused = line.read (shownPredelay);

            for (auto& allpass : diffusers[side])
                diffused = allpass.process (diffused);

            in[side] = diffused;
        }

        // The network: each line's output, modulated, damped and faded, mixed into every line's input.
        const auto along = (float) (i + 1) / (float) block.numSamples;
        const auto scale = startScale + (endScale - startScale) * along;
        std::array<float, numLines> out {};
        auto sum = 0.0f;

        for (size_t line = 0; line < (size_t) numLines; ++line)
        {
            auto& phase = phases[line];
            phase += phaseSteps[line];
            phase -= std::floor (phase);

            const auto delay = lengths[line] / startScale * scale + modulationDepth * (1.0f + dsp::sineOfPhase (phase));
            auto& smooth = damped[line];
            smooth += (lines[line].read (delay) - smooth) * dampingCoefficient;
            out[line] = smooth * losses[line];
            sum += out[line];
        }

        // A Householder matrix: each line gets its own output less a quarter of them all, which keeps the energy.
        const auto shared = sum * (2.0f / (float) numLines);
        auto left = 0.0f, right = 0.0f;

        for (size_t line = 0; line < (size_t) numLines; ++line)
        {
            lines[line].push (out[line] - shared + in[line % 2] * lineScale);
            left += leftSigns[line] * out[line];
            right += rightSigns[line] * out[line];
        }

        // Out: as wide as the width, without rumble.
        const auto mid = 0.5f * (left + right), sideways = 0.5f * (left - right) * spread;
        left = lowCuts[0].process ((mid + sideways) * outputGain, cut);
        right = lowCuts[1].process ((mid - sideways) * outputGain, cut);

        block.channels[0][i] = left;

        if (numChannels > 1)
            block.channels[1][i] = right;
    }
}
} // namespace phazite::devices
