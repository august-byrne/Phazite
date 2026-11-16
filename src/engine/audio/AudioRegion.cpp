// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AudioRegion.h"
#include "Resampler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

namespace phazite::engine
{
namespace
{
    constexpr int jumpFade = 40; // samples a region fades in over after a jump, rather than click

    // How far ahead of where the song plays a region's start is made ready for (prepare()), in seconds.
    constexpr double prepareAheadSeconds = 0.5;

    bool isSameRate (double fileRate, double engineRate) noexcept
    {
        return engineRate <= 0.0 || std::abs (fileRate / engineRate - 1.0) < 1.0e-9;
    }

    // A channel of a file from a sample on into a destination, silence where nothing is read yet or the file has none;
    // whether all of it that the file has was read.
    template <typename Sample>
    bool readFile (AudioFile& file, int channel, std::int64_t position, Sample* destination, int numSamples) noexcept
    {
        auto allRead = true;

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
            {
                std::copy (samples, samples + count, destination + done);
            }
            else
            {
                std::fill (destination + done, destination + done + count, Sample());
                allRead = false;
            }

            done += count;
        }

        return allRead;
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
        warmSerial = 0;
        warmed = false;
    }

    // Ahead of a sample of the file it will play from next (known bug 85): from there afresh, given so many of the file's
    // samples a block until its filter's start is through and it gives out its first; whether it is ready to play from
    // there. A part not read yet waits for the next block, rather than fill it with silence. The same as restart() and
    // convert() from there would make it, to the last bit: the resampler's output doesn't depend on how its input comes.
    bool warm (AudioFile& file, std::int64_t fileSample, int budget) noexcept
    {
        if (warmSerial != file.getSerial() || warmFrom != fileSample)
        {
            restart (fileSample);
            warmSerial = file.getSerial();
            warmFrom = fileSample;
        }

        const auto numChannels = (int) resamplers.size();

        for (auto given = 0; ! warmed && budget > 0; budget -= given)
        {
            given = std::min (budget, maximumInput);

            // (Before the file's start and past its end, silence is what plays there anyway; a part spans two chunks at most.)
            const auto first = std::max<std::int64_t> (0, nextInput), last = std::min (nextInput + given, file.getLength()) - 1;

            if (first <= last && (! file.isRead (first) || ! file.isRead (last)))
                return false;

            auto out = 0;

            for (int channel = 0; channel < numChannels; ++channel)
            {
                readFile (file, channel, nextInput, input.data(), given);
                const double* output = nullptr;
                out = resamplers[(size_t) channel]->process (input.data(), given, output);
                std::copy (output, output + out, left[(size_t) channel].data());
            }

            numLeft = out;
            nextInput += given;
            warmed = out > 0;
        }

        return warmed;
    }

    bool isWarmFor (const AudioFile& file, std::int64_t fileSample) const noexcept
    {
        return warmed && warmSerial == file.getSerial() && warmFrom == fileSample;
    }

    // Fills a destination for each of its channels with so many samples at the engine's rate, from where it is; whether
    // all it took of the file was read. A render reads each part of the file first.
    bool convert (AudioFile& file, float* const* destinations, int count, bool isRender) noexcept
    {
        const auto numChannels = (int) resamplers.size();
        auto done = 0;
        auto allRead = true;

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
                allRead = readFile (file, channel, nextInput, input.data(), numInput) && allRead;
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

        return allRead;
    }

    std::vector<std::unique_ptr<Resampler>> resamplers;
    std::vector<double> input;               // a channel's part of the file, as the resamplers take it
    std::array<std::vector<double>, 2> left; // what each gave out beyond what a block played
    int numLeft = 0;
    std::int64_t nextInput = 0; // the file's sample read next

    // What it was warmed for (warm()): the file, by its serial (never by its address, which a file made later can have), and
    // its sample; and whether it gives out from there now.
    std::uint64_t warmSerial = 0;
    std::int64_t warmFrom = -1;
    bool warmed = false;
};

RegionState::RegionState() = default;

RegionState::RegionState (const AudioFile& file, double engineRate)
{
    if (! isSameRate (file.getSampleRate(), engineRate))
    {
        conversion = std::make_unique<Conversion> (file.getSampleRate(), engineRate, std::min (2, file.getNumChannels()));
        spare = std::make_unique<Conversion> (file.getSampleRate(), engineRate, std::min (2, file.getNumChannels()));
    }
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

void AudioRegion::expect (std::int64_t songSample, double engineRate) const noexcept
{
    if (file != nullptr && songSample >= start && songSample < getEnd())
        file->expect ((std::int64_t) (fileStart + (double) (songSample - start) * file->getSampleRate() / engineRate));
}

void AudioRegion::prepare (std::int64_t songSample, bool playing, std::span<const std::int64_t> places, double engineRate,
                           int numSamples) const noexcept
{
    if (file == nullptr || state == nullptr || state->spare == nullptr || ! state->playsAt (*file, engineRate))
        return;

    // Where it plays from next without playing on to there: its start, a moment ahead of where the song plays next; or else
    // the first place given that it plays at. None where it plays on to anyway.
    const auto ahead = (std::int64_t) (prepareAheadSeconds * engineRate);
    std::optional<std::int64_t> next;

    if (playing && start >= songSample && start - songSample <= ahead)
        next = start;

    for (size_t i = 0; i < places.size() && ! next.has_value(); ++i)
        if (places[i] >= start && places[i] < getEnd())
            next = places[i];

    if (! next.has_value() || (state->playedTo == *next && state->playedFile == file.get()))
        return;

    // As much of the file each block as it plays in one, and no less than a quarter of what the resampler takes at a time;
    // from the file's sample play() starts it from there (the same sum, to the last bit).
    const auto budget =
        std::max (RegionState::Conversion::maximumInput / 4, (int) std::ceil (numSamples * file->getSampleRate() / engineRate));
    state->spare->warm (*file, std::llround (fileStart + (double) (*next - start) * file->getSampleRate() / engineRate), budget);
}

bool AudioRegion::needsPreparing (std::int64_t songSample, double engineRate) const noexcept
{
    if (file == nullptr || state == nullptr || state->spare == nullptr || ! state->playsAt (*file, engineRate) || songSample < start
        || songSample >= getEnd() || (state->playedTo == songSample && state->playedFile == file.get())) // plays on there
        return false;

    const auto fileSample = std::llround (fileStart + (double) (songSample - start) * file->getSampleRate() / engineRate);

    if (fileSample >= 0 && fileSample < file->getLength() && ! file->isRead (fileSample))
        return false; // never waited for: it fades in once it is read

    return ! state->spare->isWarmFor (*file, fileSample);
}

void AudioRegion::want (std::int64_t songSample, double engineRate) const
{
    if (file != nullptr && songSample >= start && songSample < getEnd())
        file->want ((std::int64_t) (fileStart + (double) (songSample - start) * file->getSampleRate() / engineRate));
}

void AudioRegion::play (float* const* channels, std::int64_t songSample, int numSamples, double engineRate, bool isRender,
                        juce::AudioBuffer<float>& scratch, Cut cut) const noexcept
{
    // The part of the stretch it plays in: up to a cut, if it is cut.
    const auto isCut = cut.length > 0 && getEnd() > cut.at + 1;
    const auto from = std::max (songSample, start), to = std::min ({ songSample + numSamples, getEnd(), isCut ? cut.at : getEnd() });

    if (to <= from || file == nullptr || scratch.getNumChannels() < 2 || scratch.getNumSamples() < to - from)
        return;

    auto& playing = *state;

    // A state made for another rate than its file's or the engine's plays as silence (the model makes each for both).
    if (! playing.playsAt (*file, engineRate))
        return;

    const auto offset = (int) (from - songSample);
    const auto count = (int) (to - from);

    // In a stop's fade, only what was playing up to here plays on.
    if (cut.continuesOnly && playing.playedTo != from)
        return;

    // A jump, a start other than its own (playback started inside it), or the region moved under the playhead, slipped in
    // its file or played from another (rendered again): it fades in, from where the file is there.
    if (playing.playedTo != from || playing.playedFile != file.get() || playing.playedStart != start
        || ! juce::exactlyEqual (playing.playedFileStart, fileStart))
    {
        playing.fadingIn = from != start ? jumpFade : 0;
        playing.missed = false; // what it missed was somewhere else
        playing.filePosition = fileStart + (double) (from - start) * file->getSampleRate() / engineRate;
        playing.playedFile = file.get();
        playing.playedStart = start;
        playing.playedFileStart = fileStart;

        // A file at another rate: through the resampling made ready for here, if it was (prepare()), else from nothing.
        if (playing.conversion != nullptr)
        {
            if (const auto fileSample = std::llround (playing.filePosition);
                playing.spare != nullptr && playing.spare->isWarmFor (*file, fileSample))
            {
                std::swap (playing.conversion, playing.spare);
                playing.spare->warmSerial = 0; // what played before, of no use now: warmed afresh for the next
            }
            else
            {
                playing.conversion->restart (fileSample);
            }
        }
    }

    playing.playedTo = to;

    // What the file plays here, on two channels (a mono file's on both).
    float* sound[] = { scratch.getWritePointer (0), scratch.getWritePointer (1) };
    const auto fileChannels = file->getNumChannels();
    auto allRead = true;

    if (playing.conversion == nullptr)
    {
        const auto first = (std::int64_t) std::llround (playing.filePosition);

        if (isRender)
            file->readNow (first, count);

        for (int channel = 0; channel < 2; ++channel)
            allRead = readFile (*file, std::min (channel, fileChannels - 1), first, sound[channel], count) && allRead;

        playing.filePosition += count;
    }
    else
    {
        allRead = playing.conversion->convert (*file, sound, count, isRender);
        playing.filePosition = (double) playing.conversion->nextInput;

        if (fileChannels < 2)
            std::copy (sound[0], sound[0] + count, sound[1]);
    }

    // A part of the file not read yet (a jump the reading thread couldn't see coming): the whole stretch is silent, and the
    // first that is all there fades in as after a jump, rather than cut in.
    if (! allRead)
    {
        playing.missed = true;
        return;
    }

    if (std::exchange (playing.missed, false))
        playing.fadingIn = jumpFade;

    // At its gain, through its fades, fading in after a jump, and out before a cut.
    const auto cutFrom = isCut ? cut.at - cut.length : getEnd();
    const auto plain = fadeIn <= (from - start) && (to - start) <= length - fadeOut && playing.fadingIn == 0 && to <= cutFrom;

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

            if (from + i >= cutFrom)
                g *= (float) (cut.at - (from + i)) / (float) (cut.length + 1);

            destination[i] += sound[channel][i] * g;
        }
    }

    playing.fadingIn = std::max (0, playing.fadingIn - count);
}
} // namespace phazite::engine
