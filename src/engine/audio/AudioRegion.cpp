// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AudioRegion.h"

#include <algorithm>
#include <cmath>

namespace phazite::engine
{
namespace
{
    constexpr int jumpFade = 40; // samples a region fades in over after a jump, rather than click

    // A channel of a file from a sample on into a destination, silence where nothing is read yet or the file has none.
    void readFile (AudioFile& file, int channel, std::int64_t position, float* destination, int numSamples) noexcept
    {
        for (int done = 0; done < numSamples;)
        {
            const auto at = position + done;

            if (at < 0 || at >= file.getLength())
            {
                const auto outside = at < 0 ? (int) std::min<std::int64_t> (-at, numSamples - done) : numSamples - done;
                juce::FloatVectorOperations::clear (destination + done, outside);
                done += outside;
                continue;
            }

            auto count = numSamples - done;

            if (const auto* samples = file.getSamples (channel, at, count))
                juce::FloatVectorOperations::copy (destination + done, samples, count);
            else
                juce::FloatVectorOperations::clear (destination + done, count);

            done += count;
        }
    }
} // namespace

float getFadeGain (FadeShape shape, float along) noexcept
{
    const auto alpha = std::clamp (along, 0.0f, 1.0f);
    const auto quarter = alpha * juce::MathConstants<float>::halfPi;

    switch (shape)
    {
        case FadeShape::linear:
            return alpha;
        case FadeShape::convex:
            return std::sin (quarter);
        case FadeShape::concave:
            return 1.0f - std::cos (quarter);
        case FadeShape::sCurve:
            return (1.0f - alpha) * (1.0f - std::cos (quarter)) + alpha * std::sin (quarter);
    }

    return alpha;
}

void AudioRegion::play (float* const* channels, std::int64_t songSample, int numSamples, double engineRate, bool isRender,
                        juce::AudioBuffer<float>& scratch) const noexcept
{
    // The part of the stretch it plays in.
    const auto from = std::max (songSample, start), to = std::min (songSample + numSamples, getEnd());

    if (to <= from || file == nullptr)
        return;

    auto& playing = *state;
    const auto offset = (int) (from - songSample);
    const auto count = (int) (to - from);
    const auto ratio = file->getSampleRate() / engineRate;
    const auto sameRate = std::abs (ratio - 1.0) < 1.0e-9;

    // A jump, or a start other than its own (playback started inside it): it fades in, from where the file is there.
    if (playing.playedTo != from)
    {
        playing.fadingIn = from != start ? jumpFade : 0;
        playing.filePosition = fileStart + (double) (from - start) * ratio;

        for (auto& interpolator : playing.interpolators)
            interpolator.reset();
    }

    playing.playedTo = to;

    // What the file plays here, on two channels (a mono file's on both).
    float* sound[] = { scratch.getWritePointer (0), scratch.getWritePointer (1) };
    const auto fileChannels = file->getNumChannels();

    if (sameRate)
    {
        const auto first = (std::int64_t) std::llround (playing.filePosition);

        if (isRender)
            file->readNow (first, count);

        for (int channel = 0; channel < 2; ++channel)
            readFile (*file, std::min (channel, fileChannels - 1), first, sound[channel], count);

        playing.filePosition += count;
    }
    else
    {
        // Through 5-point Lagrange interpolation, which keeps its own place between the samples it was given: the file's
        // samples from where it is, as many as the output needs, read after the output in the scratch buffer.
        const auto first = (std::int64_t) std::floor (playing.filePosition);
        const auto needed = (int) std::ceil (count * ratio) + 8;
        const auto inputAt = scratch.getNumSamples() - needed;

        if (inputAt < count) // a file at more than 8 times the engine's rate: too much to read in a block
            return;

        if (isRender)
            file->readNow (first, needed);

        auto used = 0;

        for (int channel = 0; channel < 2; ++channel)
        {
            auto* input = scratch.getWritePointer (channel, inputAt);
            readFile (*file, std::min (channel, fileChannels - 1), first, input, needed);
            used = playing.interpolators[(size_t) channel].process (ratio, input, sound[channel], count);
        }

        playing.filePosition = (double) first + used;
    }

    // At its gain, through its fades, and fading in after a jump.
    const auto plain = fadeIn <= (from - start) && (to - start) <= length - fadeOut && playing.fadingIn == 0;

    for (int channel = 0; channel < 2; ++channel)
    {
        auto* destination = channels[channel] + offset;

        if (plain)
        {
            juce::FloatVectorOperations::addWithMultiply (destination, sound[channel], gain, count);
            continue;
        }

        auto fadingIn = playing.fadingIn;

        for (int i = 0; i < count; ++i)
        {
            const auto t = (from - start) + i;
            auto g = gain;

            if (t < fadeIn)
                g *= getFadeGain (fadeInShape, (float) t / (float) fadeIn);

            if (t >= length - fadeOut)
                g *= getFadeGain (fadeOutShape, (float) (length - t) / (float) fadeOut);

            if (fadingIn > 0)
                g *= (float) (jumpFade - --fadingIn) / (float) jumpFade;

            destination[i] += sound[channel][i] * g;
        }
    }

    playing.fadingIn = std::max (0, playing.fadingIn - count);
}
} // namespace phazite::engine
