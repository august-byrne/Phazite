// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Stretch.h"

#if JUCE_MSVC
 #pragma warning(push, 0)
#endif
#include <rubberband/RubberBandStretcher.h>
#if JUCE_MSVC
 #pragma warning(pop)
#endif

#include <algorithm>
#include <cmath>

namespace phazite::stretch
{
namespace
{
    using Stretcher = RubberBand::RubberBandStretcher;

    constexpr int blockFrames = 4096;

    // The stretches a time map's hops take: Phazite's tempos (20 to 300 BPM) keep well within them.
    constexpr double minRatio = 1.0 / 16.0, maxRatio = 16.0;

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

    // What a stretcher gives, handed on to a sink up to the frames wanted: after leaving out its first so many (real-time
    // mode's start delay), and with silence after it if it gives fewer, as Rubber Band gives about as many frames as asked.
    class Output
    {
    public:
        Output (const Sink& s, int numChannels, juce::int64 framesWanted, juce::int64 framesToSkip = 0)
            : sink (s),
              block (numChannels, blockFrames),
              wanted (framesWanted),
              toSkip (framesToSkip)
        {
        }

        // All it has ready; false if the sink stopped it.
        bool take (Stretcher& stretcher)
        {
            for (auto available = stretcher.available(); available > 0; available = stretcher.available())
            {
                const auto frames = (int) std::min ((juce::int64) std::min (available, blockFrames), toSkip > 0 ? toSkip : blockFrames);
                stretcher.retrieve (block.getArrayOfWritePointers(), (size_t) frames);

                if (toSkip > 0)
                {
                    toSkip -= frames;
                    continue;
                }

                const auto kept = (int) std::min ((juce::int64) frames, wanted - given);

                if (kept > 0 && ! sink (block, kept))
                    return false;

                given += std::max (0, kept);
            }

            return true;
        }

        bool fillWithSilence()
        {
            block.clear();

            while (given < wanted)
            {
                const auto frames = (int) std::min ((juce::int64) blockFrames, wanted - given);

                if (! sink (block, frames))
                    return false;

                given += frames;
            }

            return true;
        }

        bool isComplete() const noexcept { return given >= wanted; }

    private:
        const Sink& sink;
        juce::AudioBuffer<float> block;
        const juce::int64 wanted;
        juce::int64 toSkip, given = 0;
    };

    // Rubber Band's R3 engine, offline. Told the sound's length, it needs no pass to study it first: R3's study only counts
    // the frames.
    bool stretch (const Source& source, const Settings& settings, const Sink& sink, const std::function<bool()>& shouldStop)
    {
        auto options = Stretcher::OptionProcessOffline | Stretcher::OptionEngineFiner;

        if (source.numChannels == 2)
            options |= Stretcher::OptionChannelsTogether; // keeps a stereo sound's image

        // Played at another rate, more frames each a little lower, which that rate brings back up.
        Stretcher stretcher ((size_t) std::lround (source.sampleRate), (size_t) source.numChannels, options,
                             settings.timeRatio * settings.rateRatio, std::pow (2.0, settings.semitones / 12.0) / settings.rateRatio);
        stretcher.setExpectedInputDuration ((size_t) source.length);
        stretcher.setMaxProcessSize ((size_t) blockFrames);

        juce::AudioBuffer<float> in (source.numChannels, blockFrames);
        Output output (sink, source.numChannels, getStretchedLength (source.length, settings));

        for (juce::int64 start = 0; start < source.length; start += blockFrames)
        {
            if (shouldStop != nullptr && shouldStop())
                return false;

            const auto frames = (int) std::min ((juce::int64) blockFrames, source.length - start);
            source.read (in, start, frames);
            stretcher.process (in.getArrayOfReadPointers(), (size_t) frames, start + frames >= source.length);

            if (! output.take (stretcher))
                return false;
        }

        return output.take (stretcher) && output.fillWithSilence();
    }

    // Where each frame of a sound plays in what is rendered from it, in frames of each, as a time map says (Settings): from
    // the start of both, straight from one of its times to the next, and on past its last as its last part goes.
    class FrameMap
    {
    public:
        FrameMap (const std::vector<std::pair<double, double>>& timeMap, double inRate, double outRate, double ratio)
        {
            for (const auto& [in, out] : timeMap)
                if (in * inRate > points.back().first)
                    points.push_back ({ in * inRate, out * outRate });

            if (points.size() < 2)
                points.push_back ({ inRate, ratio * outRate });
        }

        double operator() (double frame) const
        {
            const auto next = std::upper_bound (points.begin() + 1, points.end() - 1, frame,
                                                [] (double f, const std::pair<double, double>& point) { return f < point.first; });
            const auto& [x0, y0] = *(next - 1);
            const auto& [x1, y1] = *next;
            return y0 + (frame - x0) * (y1 - y0) / (x1 - x0);
        }

    private:
        std::vector<std::pair<double, double>> points { { 0.0, 0.0 } };
    };

    // A stretch that changes along the sound (its time map): R3 in real-time mode, which takes a new ratio at each hop, the
    // ratios closed round the map. Each call to process() gives it just what its next hop reads, which tells where in the
    // sound that hop is: so many frames given, less what it holds, a window less what it asks for (getSamplesRequired()),
    // after half a window of silence (getPreferredStartPad()), which centres its first hop on the sound's start. Each hop's
    // ratio is set to bring where its output comes to, as the ratios before it took it (as its own reckoning goes, which it
    // keeps its output to), to where the map says a hop on: any drift is taken up at the next hop. (Its offline key frames
    // drifted, up to 9 ms over a ramp that doubled the stretch: it took each up a hop late, and never made up for it.)
    bool stretchAlongMap (const Source& source, const Settings& settings, const Sink& sink, const std::function<bool()>& shouldStop)
    {
        const auto pitch = std::pow (2.0, settings.semitones / 12.0) / settings.rateRatio;
        auto options = Stretcher::OptionProcessRealTime | Stretcher::OptionEngineFiner;

        if (source.numChannels == 2)
            options |= Stretcher::OptionChannelsTogether; // keeps a stereo sound's image

        // Pitched, or played at another rate, it resamples after it stretches, as offline mode does: its hops read the
        // sound's own frames.
        if (! juce::exactlyEqual (pitch, 1.0))
            options |= Stretcher::OptionPitchHighConsistency;

        const FrameMap map (settings.timeMap, source.sampleRate, source.sampleRate * settings.rateRatio, settings.timeRatio);
        constexpr double firstHop = 256.0; // about how far a hop reads at first; then as far as the last one did
        Stretcher stretcher ((size_t) std::lround (source.sampleRate), (size_t) source.numChannels, options,
                             juce::jlimit (minRatio, maxRatio, map (firstHop) / firstHop), pitch);

        const auto window = (int) stretcher.getSamplesRequired(); // all it reads for its first hop
        const auto pad = (int) stretcher.getPreferredStartPad();
        stretcher.setMaxProcessSize ((size_t) window);

        juce::AudioBuffer<float> in (source.numChannels, window);
        in.clear();
        stretcher.process (in.getArrayOfReadPointers(), (size_t) pad, false);

        Output output (sink, source.numChannels, getStretchedLength (source.length, settings), (juce::int64) stretcher.getStartDelay());
        juce::int64 given = 0;
        double centre = 0.0, planned = 0.0, hop = firstHop, ratio = 1.0;

        // Past the sound's end, silence brings out what it holds.
        const auto lastToGive = source.length + 8 * (juce::int64) window;

        while (! output.isComplete() && given < lastToGive)
        {
            if (shouldStop != nullptr && shouldStop())
                return false;

            const auto required = std::max (1, (int) stretcher.getSamplesRequired());

            if (const auto next = (double) (pad + given - (window - required)); next > centre)
            {
                planned += (next - centre) * ratio;
                hop = next - centre;
                centre = next;
            }

            ratio = juce::jlimit (minRatio, maxRatio, (map (centre + hop) - planned) / hop);
            stretcher.setTimeRatio (ratio);

            source.read (in, given, required);
            stretcher.process (in.getArrayOfReadPointers(), (size_t) required, false);
            given += required;

            if (! output.take (stretcher))
                return false;
        }

        return output.fillWithSilence();
    }
} // namespace

bool isUnchanged (const Settings& settings)
{
    return juce::exactlyEqual (settings.timeRatio, 1.0) && juce::exactlyEqual (settings.semitones, 0.0) && ! settings.reversed
           && juce::exactlyEqual (settings.rateRatio, 1.0) && settings.timeMap.empty();
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

    if (! settings.timeMap.empty())
        return stretchAlongMap (played, settings, sink, shouldStop);

    if (juce::exactlyEqual (settings.timeRatio, 1.0) && juce::exactlyEqual (settings.semitones, 0.0)
        && juce::exactlyEqual (settings.rateRatio, 1.0))
        return copy (played, sink, shouldStop);

    return stretch (played, settings, sink, shouldStop);
}
} // namespace phazite::stretch
