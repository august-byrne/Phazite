// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Stretch.h"

#if JUCE_MSVC
 #pragma warning(push, 0)
#endif
#include <rubberband/RubberBandStretcher.h>
#if JUCE_MSVC
 #pragma warning(pop)
#endif

#include <cmath>

namespace phazite::stretch
{
namespace
{
    constexpr int blockFrames = 4096;

    // A source read backwards: its last frame first.
    Source reversedSource (const Source& source)
    {
        auto reversed = source;
        reversed.read = [source, scratch = std::make_shared<juce::AudioBuffer<float>>()] (juce::AudioBuffer<float>& buffer,
                                                                                          juce::int64 start, int numFrames)
        {
            // Frames start.. of the reversed sound are the source's frames length - 1 - start downwards.
            const auto end = source.length - start;
            const auto from = std::max ((juce::int64) 0, end - numFrames);
            const auto count = (int) std::max ((juce::int64) 0, end - from);
            buffer.clear (0, numFrames);

            if (count <= 0)
                return;

            scratch->setSize (buffer.getNumChannels(), count, false, false, true);
            source.read (*scratch, from, count);

            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            {
                const auto* in = scratch->getReadPointer (channel);
                auto* out = buffer.getWritePointer (channel);

                for (int i = 0; i < count; ++i)
                    out[i] = in[count - 1 - i];
            }
        };
        return reversed;
    }

    // The sound as it is, or backwards, in blocks.
    bool copy (const Source& source, const Sink& sink, const std::function<bool()>& shouldStop)
    {
        juce::AudioBuffer<float> block (source.numChannels, blockFrames);

        for (juce::int64 start = 0; start < source.length; start += blockFrames)
        {
            if (shouldStop != nullptr && shouldStop())
                return false;

            const auto frames = (int) std::min ((juce::int64) blockFrames, source.length - start);
            source.read (block, start, frames);

            if (! sink (block, frames))
                return false;
        }

        return true;
    }

    // Rubber Band's R3 engine, offline. Told the sound's length, it needs no pass to study it first: R3's study only counts
    // the frames.
    bool stretch (const Source& source, const Settings& settings, const Sink& sink, const std::function<bool()>& shouldStop)
    {
        using Stretcher = RubberBand::RubberBandStretcher;
        auto options = Stretcher::OptionProcessOffline | Stretcher::OptionEngineFiner;

        if (source.numChannels == 2)
            options |= Stretcher::OptionChannelsTogether; // keeps a stereo sound's image

        // Played at another rate, more frames each a little lower, which that rate brings back up.
        Stretcher stretcher ((size_t) std::lround (source.sampleRate), (size_t) source.numChannels, options,
                             settings.timeRatio * settings.rateRatio, std::pow (2.0, settings.semitones / 12.0) / settings.rateRatio);
        stretcher.setExpectedInputDuration ((size_t) source.length);
        stretcher.setMaxProcessSize ((size_t) blockFrames);

        juce::AudioBuffer<float> in (source.numChannels, blockFrames), out (source.numChannels, blockFrames);
        const auto wanted = getStretchedLength (source.length, settings);
        juce::int64 given = 0;

        // Hands the sink what the stretcher has ready, up to the length wanted.
        const auto takeOutput = [&]
        {
            for (auto available = stretcher.available(); available > 0; available = stretcher.available())
            {
                const auto frames = std::min (available, blockFrames);
                stretcher.retrieve (out.getArrayOfWritePointers(), (size_t) frames);
                const auto kept = (int) std::min ((juce::int64) frames, wanted - given);

                if (kept > 0 && ! sink (out, kept))
                    return false;

                given += std::max (0, kept);
            }

            return true;
        };

        for (juce::int64 start = 0; start < source.length; start += blockFrames)
        {
            if (shouldStop != nullptr && shouldStop())
                return false;

            const auto frames = (int) std::min ((juce::int64) blockFrames, source.length - start);
            source.read (in, start, frames);
            stretcher.process (in.getArrayOfReadPointers(), (size_t) frames, start + frames >= source.length);

            if (! takeOutput())
                return false;
        }

        if (! takeOutput())
            return false;

        // Rubber Band gives about as many frames as asked; any it leaves short are silence.
        out.clear();

        while (given < wanted)
        {
            const auto frames = (int) std::min ((juce::int64) blockFrames, wanted - given);

            if (! sink (out, frames))
                return false;

            given += frames;
        }

        return true;
    }
} // namespace

bool isUnchanged (const Settings& settings)
{
    return juce::exactlyEqual (settings.timeRatio, 1.0) && juce::exactlyEqual (settings.semitones, 0.0) && ! settings.reversed
           && juce::exactlyEqual (settings.rateRatio, 1.0);
}

juce::int64 getStretchedLength (juce::int64 frames, const Settings& settings)
{
    return (juce::int64) std::llround ((double) frames * settings.timeRatio * settings.rateRatio);
}

bool render (const Source& source, const Settings& settings, const Sink& sink, const std::function<bool()>& shouldStop)
{
    if (source.numChannels <= 0 || source.length <= 0 || source.sampleRate <= 0.0 || settings.timeRatio <= 0.0 || settings.rateRatio <= 0.0)
        return true;

    const auto played = settings.reversed ? reversedSource (source) : source;

    if (juce::exactlyEqual (settings.timeRatio, 1.0) && juce::exactlyEqual (settings.semitones, 0.0)
        && juce::exactlyEqual (settings.rateRatio, 1.0))
        return copy (played, sink, shouldStop);

    return stretch (played, settings, sink, shouldStop);
}
} // namespace phazite::stretch
