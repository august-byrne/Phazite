// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AudioRegion.h"
#include "Resampler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace phazite::engine
{
namespace
{
    constexpr int jumpFade = 40; // samples a region fades in over after a jump, rather than click

    bool isSameRate (double fileRate, double engineRate) noexcept
    {
        return engineRate <= 0.0 || std::abs (fileRate / engineRate - 1.0) < 1.0e-9;
    }

    // A channel of a file from a sample on into a destination, silence where nothing is read yet or the file has none.
    template <typename Sample>
    void readFile (AudioFile& file, int channel, std::int64_t position, Sample* destination, int numSamples) noexcept
    {
        for (int done = 0; done < numSamples;)
        {
            const auto at = position + done;

            if (at < 0 || at >= file.getLength())
            {
                const auto outside = at < 0 ? (int) std::min<std::int64_t> (-at, numSamples - done) : numSamples - done;
                std::fill (destination + done, destination + done + outside, Sample());
                done += outside;
                continue;
            }

            auto count = numSamples - done;

            if (const auto* samples = file.getSamples (channel, at, count))
                std::copy (samples, samples + count, destination + done);
            else
                std::fill (destination + done, destination + done + count, Sample());

            done += count;
        }
    }

    void copyToFloat (const double* source, int numSamples, float* destination) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
            destination[i] = (float) source[i];
    }
} // namespace

// A file at another rate than the engine's: each of its channels (two at most) through a resampler, given the file's
// samples a part at a time from where it reads next; what a part gives out beyond what a block plays waits for the next.
struct RegionState::Conversion
{
    static constexpr int maximumInput = 1024; // of the file's samples, given to a resampler at a time
    static constexpr int maximumRounds = 256; // parts in a block: more than a file at 8 times the rate needs after a jump

    Conversion (double fileRate, double engineRate, int numChannels)
    {
        for (int channel = 0; channel < numChannels; ++channel)
            resamplers.push_back (std::make_unique<Resampler> (fileRate, engineRate, maximumInput));

        input.assign ((size_t) maximumInput, 0.0);

        for (int channel = 0; channel < numChannels; ++channel)
            left[(size_t) channel].assign ((size_t) resamplers.front()->getMaximumOutput(), 0.0);
    }

    bool isFor (double fileRate, double engineRate, int numChannels) const noexcept
    {
        return juce::exactlyEqual (resamplers.front()->getFromRate(), fileRate)
               && juce::exactlyEqual (resamplers.front()->getToRate(), engineRate) && (int) resamplers.size() == numChannels;
    }

    // From a sample of the file, after a jump.
    void restart (std::int64_t fileSample) noexcept
    {
        for (auto& resampler : resamplers)
            resampler->reset();

        numLeft = 0;
        nextInput = fileSample;
    }

    // Fills a destination for each of its channels with so many samples at the engine's rate, from where it is. A render
    // reads each part of the file first.
    void convert (AudioFile& file, float* const* destinations, int count, bool isRender) noexcept
    {
        const auto numChannels = (int) resamplers.size();
        auto done = 0;

        if (numLeft > 0)
        {
            done = std::min (numLeft, count);

            for (int channel = 0; channel < numChannels; ++channel)
            {
                auto* waiting = left[(size_t) channel].data();
                copyToFloat (waiting, done, destinations[channel]);
                std::copy (waiting + done, waiting + numLeft, waiting);
            }

            numLeft -= done;
        }

        const auto ratio = resamplers.front()->getFromRate() / resamplers.front()->getToRate();

        for (int round = 0; done < count && round < maximumRounds; ++round)
        {
            const auto wanted = count - done;
            const auto numInput = std::clamp ((int) std::ceil (wanted * ratio), 1, maximumInput);
            auto given = 0;

            if (isRender)
                file.readNow (nextInput, numInput);

            for (int channel = 0; channel < numChannels; ++channel)
            {
                readFile (file, channel, nextInput, input.data(), numInput);
                const double* output = nullptr;
                given = resamplers[(size_t) channel]->process (input.data(), numInput, output);
                const auto used = std::min (given, wanted);
                copyToFloat (output, used, destinations[channel] + done);
                std::copy (output + used, output + given, left[(size_t) channel].data());
            }

            numLeft = given - std::min (given, wanted);
            done += std::min (given, wanted);
            nextInput += numInput;
        }

        for (int channel = 0; channel < numChannels; ++channel)
            std::fill (destinations[channel] + done, destinations[channel] + count, 0.0f);
    }

    std::vector<std::unique_ptr<Resampler>> resamplers;
    std::vector<double> input;               // a channel's part of the file, as the resamplers take it
    std::array<std::vector<double>, 2> left; // what each gave out beyond what a block played
    int numLeft = 0;
    std::int64_t nextInput = 0; // the file's sample read next
};

RegionState::RegionState() = default;

RegionState::RegionState (const AudioFile& file, double engineRate)
{
    if (! isSameRate (file.getSampleRate(), engineRate))
        conversion = std::make_unique<Conversion> (file.getSampleRate(), engineRate, std::min (2, file.getNumChannels()));
}

RegionState::~RegionState() = default;

bool RegionState::playsAt (const AudioFile& file, double engineRate) const noexcept
{
    if (isSameRate (file.getSampleRate(), engineRate))
        return conversion == nullptr;

    return conversion != nullptr && conversion->isFor (file.getSampleRate(), engineRate, std::min (2, file.getNumChannels()));
}

float getFadeGain (FadeCurve curve, float along) noexcept
{
    const auto alpha = std::clamp (along, 0.0f, 1.0f);
    const auto bend = std::clamp (curve.bend, -1.0f, 1.0f);

    if (juce::exactlyEqual (bend, 0.0f))
        return alpha;

    // The shape bent all the way: a quarter of a sine wave, quick at first (convex), or its mirror image, slow at first
    // (concave); a raised cosine, half a cosine wave, slow at both ends (the S-curve), or bent the other way the line's
    // mirror image of it, quick at both ends.
    auto bent = 0.0f;

    if (curve.shape == FadeShape::curve)
    {
        const auto quarter = alpha * juce::MathConstants<float>::halfPi;
        bent = bend > 0.0f ? std::sin (quarter) : 1.0f - std::cos (quarter);
    }
    else
    {
        const auto s = 0.5f - 0.5f * std::cos (alpha * juce::MathConstants<float>::pi);
        bent = bend > 0.0f ? s : 2.0f * alpha - s;
    }

    // Written so that bent all the way, it is that shape to the last bit.
    const auto amount = std::abs (bend);
    return (1.0f - amount) * alpha + amount * bent;
}

void AudioRegion::play (float* const* channels, std::int64_t songSample, int numSamples, double engineRate, bool isRender,
                        juce::AudioBuffer<float>& scratch) const noexcept
{
    // The part of the stretch it plays in.
    const auto from = std::max (songSample, start), to = std::min (songSample + numSamples, getEnd());

    if (to <= from || file == nullptr || scratch.getNumChannels() < 2 || scratch.getNumSamples() < to - from)
        return;

    auto& playing = *state;

    // A state made for another rate than its file's or the engine's plays as silence (the model makes each for both).
    if (! playing.playsAt (*file, engineRate))
        return;

    const auto offset = (int) (from - songSample);
    const auto count = (int) (to - from);

    // A jump, or a start other than its own (playback started inside it): it fades in, from where the file is there.
    if (playing.playedTo != from)
    {
        playing.fadingIn = from != start ? jumpFade : 0;
        playing.filePosition = fileStart + (double) (from - start) * file->getSampleRate() / engineRate;

        if (playing.conversion != nullptr)
            playing.conversion->restart (std::llround (playing.filePosition));
    }

    playing.playedTo = to;

    // What the file plays here, on two channels (a mono file's on both).
    float* sound[] = { scratch.getWritePointer (0), scratch.getWritePointer (1) };
    const auto fileChannels = file->getNumChannels();

    if (playing.conversion == nullptr)
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
        playing.conversion->convert (*file, sound, count, isRender);
        playing.filePosition = (double) playing.conversion->nextInput;

        if (fileChannels < 2)
            std::copy (sound[0], sound[0] + count, sound[1]);
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
                g *= getFadeGain (fadeInCurve, (float) t / (float) fadeIn);

            if (t >= length - fadeOut)
                g *= getFadeGain (fadeOutCurve, (float) (length - t) / (float) fadeOut);

            if (fadingIn > 0)
                g *= (float) (jumpFade - --fadingIn) / (float) jumpFade;

            destination[i] += sound[channel][i] * g;
        }
    }

    playing.fadingIn = std::max (0, playing.fadingIn - count);
}
} // namespace phazite::engine
