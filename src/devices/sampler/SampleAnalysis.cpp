// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SampleAnalysis.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
namespace
{
    constexpr double lowestPitchHz = 27.5, highestPitchHz = 4186.0; // A0 and C8
    constexpr float periodicThreshold = 0.15f;                      // YIN's: a dip below it is a period
    constexpr int pitchFrames = 8;                                  // looked at, a hop of half a window apart
    constexpr double onsetGapSeconds = 0.04;                        // two hits at least this far apart
    constexpr int envelopeBlock = 32;                               // samples, to place a hit where its rise starts

    // The sound's channels mixed.
    std::vector<float> mixDown (const juce::AudioBuffer<float>& audio)
    {
        const auto frames = audio.getNumSamples();
        const auto channels = audio.getNumChannels();
        std::vector<float> mixed ((size_t) frames, 0.0f);

        if (channels == 0)
            return mixed;

        const auto share = 1.0f / (float) channels;

        for (int channel = 0; channel < channels; ++channel)
        {
            const auto* samples = audio.getReadPointer (channel);

            for (int i = 0; i < frames; ++i)
                mixed[(size_t) i] += samples[i] * share;
        }

        return mixed;
    }

    float findPeak (const std::vector<float>& samples)
    {
        auto peak = 0.0f;

        for (const auto sample : samples)
            peak = std::max (peak, std::abs (sample));

        return peak;
    }

    int getOrder (int size)
    {
        auto order = 0;

        while ((1 << order) < size)
            ++order;

        return order;
    }

    // Whether the samples cross zero between i - 1 and i: then the one of the two nearer zero, else -1.
    int crossesAt (const std::vector<float>& samples, int i)
    {
        if (i <= 0 || i >= (int) samples.size())
            return -1;

        const auto before = samples[(size_t) i - 1], after = samples[(size_t) i];

        if ((before <= 0.0f && after >= 0.0f) || (before >= 0.0f && after <= 0.0f))
            return std::abs (before) < std::abs (after) ? i - 1 : i;

        return -1;
    }

    // The nearest zero crossing in mixed samples, within reach either way.
    int findCrossing (const std::vector<float>& samples, int sample, int reach)
    {
        const auto frames = (int) samples.size();

        if (frames == 0)
            return 0;

        sample = std::clamp (sample, 0, frames - 1);

        for (int distance = 0; distance <= reach; ++distance)
        {
            if (const auto later = crossesAt (samples, sample + distance); later >= 0)
                return later;

            if (const auto earlier = crossesAt (samples, sample - distance); earlier >= 0)
                return earlier;
        }

        return sample;
    }

    // The zero crossing at or before a sample, within reach, so that a hit starting there keeps all of its attack; the
    // sample itself if there is none.
    int findCrossingBefore (const std::vector<float>& samples, int sample, int reach)
    {
        for (int i = sample; i >= sample - reach; --i)
            if (const auto crossing = crossesAt (samples, i); crossing >= 0)
                return crossing;

        return sample;
    }

    // Where the rise of a hit found near a sample begins: the steepest rise of the sound's envelope, in blocks, between from
    // and to, back to where it started rising, and there the first sample a tenth of the way up from the level before it.
    int placeRise (const std::vector<float>& samples, int from, int to)
    {
        const auto frames = (int) samples.size();
        from = std::clamp (from, 0, frames);
        to = std::clamp (to, from, frames);
        const auto numBlocks = (to - from) / envelopeBlock;

        if (numBlocks < 2)
            return from;

        std::vector<float> envelope ((size_t) numBlocks, 0.0f);

        for (int b = 0; b < numBlocks; ++b)
            for (int i = 0; i < envelopeBlock; ++i)
                envelope[(size_t) b] = std::max (envelope[(size_t) b], std::abs (samples[(size_t) (from + b * envelopeBlock + i)]));

        // The block that rises most above the quietest of the few before it.
        auto steepest = 0;
        auto steepestRise = -1.0f;

        for (int b = 1; b < numBlocks; ++b)
        {
            auto quietest = envelope[(size_t) b - 1];

            for (int back = 2; back <= 6 && b - back >= 0; ++back)
                quietest = std::min (quietest, envelope[(size_t) (b - back)]);

            if (const auto rise = envelope[(size_t) b] - quietest; rise > steepestRise)
            {
                steepestRise = rise;
                steepest = b;
            }
        }

        // Back while it was still rising.
        auto start = steepest;

        while (start > 0 && envelope[(size_t) start - 1] < envelope[(size_t) start] * 0.9f)
            --start;

        auto floor = envelope[(size_t) start];

        for (int back = 1; back <= 6 && start - back >= 0; ++back)
            floor = std::min (floor, envelope[(size_t) (start - back)]);

        const auto threshold = floor + 0.1f * (envelope[(size_t) steepest] - floor);

        for (int i = from + start * envelopeBlock; i < from + (steepest + 1) * envelopeBlock; ++i)
            if (std::abs (samples[(size_t) i]) > threshold)
                return i;

        return from + start * envelopeBlock;
    }
} // namespace

std::optional<double> detectPitch (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    const auto frames = audio.getNumSamples();

    if (frames == 0 || sampleRate <= 0.0)
        return {};

    const auto sound = mixDown (audio);
    const auto peak = findPeak (sound);

    if (peak <= 1.0e-4f)
        return {};

    // Its steady part: from where it begins (above a tenth of its peak), past its attack: 60 ms on, or a quarter of what is
    // left for a short sound.
    auto begins = 0;

    while (begins < frames && std::abs (sound[(size_t) begins]) < 0.1f * peak)
        ++begins;

    const auto start = begins + std::min ((int) (0.06 * sampleRate), (frames - begins) / 4);

    const auto longestPeriod = (int) std::ceil (sampleRate / lowestPitchHz);
    const auto shortestPeriod = std::max (2, (int) std::floor (sampleRate / highestPitchHz));
    const auto window = juce::nextPowerOfTwo ((int) (sampleRate * 0.04)); // about 43 ms at 48 kHz
    const auto span = window + longestPeriod + 2;                         // what a frame reads
    const juce::dsp::FFT fft (getOrder (span));
    const auto size = fft.getSize();
    const auto hop = window / 2;

    std::vector<float> windowed ((size_t) size * 2), whole ((size_t) size * 2);
    std::vector<double> difference ((size_t) longestPeriod + 2, 0.0), normalized ((size_t) longestPeriod + 2, 1.0);
    std::vector<double> notes;
    std::vector<double> depths;
    auto analysed = 0;

    for (int frame = 0; frame < pitchFrames && start + frame * hop + span <= frames; ++frame)
    {
        const auto* x = sound.data() + start + frame * hop;
        auto energy = 0.0;

        for (int j = 0; j < window; ++j)
            energy += (double) x[j] * x[j];

        // Faded to -40 dB of its peak: no more to hear.
        if (energy / window < (double) peak * peak * 1.0e-4)
            break;

        ++analysed;

        // How alike the window is to itself a lag later, for every lag at once: the window's spectrum against the whole
        // span's, by FFT. Its scale is set by the lag of 0, which is the window's energy.
        std::fill (windowed.begin(), windowed.end(), 0.0f);
        std::fill (whole.begin(), whole.end(), 0.0f);
        std::copy (x, x + window, windowed.begin());
        std::copy (x, x + span, whole.begin());
        fft.performRealOnlyForwardTransform (windowed.data());
        fft.performRealOnlyForwardTransform (whole.data());

        for (int k = 0; k < size; ++k)
        {
            const auto ar = windowed[(size_t) (2 * k)], ai = windowed[(size_t) (2 * k + 1)];
            const auto br = whole[(size_t) (2 * k)], bi = whole[(size_t) (2 * k + 1)];
            windowed[(size_t) (2 * k)] = ar * br + ai * bi; // the window's spectrum's conjugate times the span's
            windowed[(size_t) (2 * k + 1)] = ar * bi - ai * br;
        }

        fft.performRealOnlyInverseTransform (windowed.data());

        if (std::abs (windowed[0]) <= 1.0e-20f)
            continue;

        const auto scale = energy / (double) windowed[0];

        // YIN's difference for each lag, from the energies of the window and of the stretch a lag later, and normalized by
        // its mean up to that lag.
        auto later = energy, sum = 0.0;
        difference[0] = 0.0;
        normalized[0] = 1.0;

        for (int lag = 1; lag <= longestPeriod; ++lag)
        {
            later += (double) x[lag + window - 1] * x[lag + window - 1] - (double) x[lag - 1] * x[lag - 1];
            difference[(size_t) lag] = std::max (0.0, energy + later - 2.0 * scale * (double) windowed[(size_t) lag]);
            sum += difference[(size_t) lag];
            normalized[(size_t) lag] = sum > 0.0 ? difference[(size_t) lag] * lag / sum : 1.0;
        }

        // The first dip below the threshold, at its lowest.
        auto found = -1;

        for (int lag = shortestPeriod; lag < longestPeriod; ++lag)
        {
            if (normalized[(size_t) lag] < periodicThreshold)
            {
                while (lag + 1 < longestPeriod && normalized[(size_t) lag + 1] < normalized[(size_t) lag])
                    ++lag;

                found = lag;
                break;
            }
        }

        if (found <= 0)
            continue;

        // Between the lags, by a parabola through the three around it.
        const auto before = normalized[(size_t) found - 1], at = normalized[(size_t) found], after = normalized[(size_t) found + 1];
        const auto curvature = before - 2.0 * at + after;
        const auto shift = std::abs (curvature) > 1.0e-12 ? std::clamp (0.5 * (before - after) / curvature, -0.5, 0.5) : 0.0;
        const auto period = found + shift;
        notes.push_back (69.0 + 12.0 * std::log2 (sampleRate / period / 440.0));
        depths.push_back (at);
    }

    if (notes.empty())
        return {};

    // Most of the frames agree, at least two of them, or the only one there was dips deep.
    auto sorted = notes;
    std::sort (sorted.begin(), sorted.end());
    const auto median = sorted[sorted.size() / 2];
    auto agreeing = 0;
    auto total = 0.0;

    for (const auto note : notes)
    {
        if (std::abs (note - median) < 0.5)
        {
            ++agreeing;
            total += note;
        }
    }

    const auto onlyOne = analysed == 1 && depths.front() < 0.1;

    if ((agreeing < 2 && ! onlyOne) || agreeing * 2 < (int) notes.size())
        return {};

    return total / agreeing;
}

std::vector<Onset> detectOnsets (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    std::vector<Onset> onsets;
    const auto frames = audio.getNumSamples();

    if (frames == 0 || sampleRate <= 0.0)
        return onsets;

    const auto sound = mixDown (audio);
    const auto peak = findPeak (sound);

    if (peak <= 1.0e-4f)
        return onsets;

    // Its first sound (above -60 dB of its peak), a millisecond before.
    auto first = 0;

    while (first < frames && std::abs (sound[(size_t) first]) < peak * 0.001f)
        ++first;

    onsets.push_back ({ std::max (0, first - (int) (0.001 * sampleRate)), 1.0f, 0.0f });

    // The spectral flux: how much the log magnitudes rise from one frame to the next, frames of about 21 ms a quarter of
    // one apart, centred on their hops.
    const auto size = juce::nextPowerOfTwo ((int) (sampleRate * 0.02));
    const auto hop = size / 4;
    const juce::dsp::FFT fft (getOrder (size));
    std::vector<float> hann ((size_t) size), data ((size_t) size * 2), previous ((size_t) size / 2 + 1, 0.0f);

    for (int j = 0; j < size; ++j)
        hann[(size_t) j] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) j / (float) size);

    const auto numFrames = frames / hop + 1;
    std::vector<float> flux ((size_t) numFrames, 0.0f);
    const auto norm = 1.0f / (peak * (float) size * 0.25f); // a full-scale sine's bin comes to about 1

    for (int n = 0; n < numFrames; ++n)
    {
        std::fill (data.begin(), data.end(), 0.0f);

        for (int j = 0; j < size; ++j)
            if (const auto at = n * hop - size / 2 + j; at >= 0 && at < frames)
                data[(size_t) j] = sound[(size_t) at] * hann[(size_t) j];

        fft.performFrequencyOnlyForwardTransform (data.data());
        auto rise = 0.0f;

        for (int k = 1; k <= size / 2; ++k)
        {
            const auto level = std::log1p (100.0f * data[(size_t) k] * norm);
            rise += std::max (0.0f, level - previous[(size_t) k]);
            previous[(size_t) k] = level;
        }

        flux[(size_t) n] = rise;
    }

    const auto largest = *std::max_element (flux.begin(), flux.end());

    if (largest <= 0.0f)
        return onsets;

    // Peaks above the flux's mean around them, by a share of the largest.
    struct Peak
    {
        int frame = 0;
        float flux = 0.0f;
    };

    std::vector<Peak> peaks;
    const auto margin = 0.07f * largest;

    for (int n = 1; n < numFrames; ++n)
    {
        auto isPeak = true;
        auto mean = 0.0f;
        auto counted = 0;

        for (int m = std::max (0, n - 8); m <= std::min (numFrames - 1, n + 3); ++m)
        {
            mean += flux[(size_t) m];
            ++counted;

            if (std::abs (m - n) <= 3 && flux[(size_t) m] > flux[(size_t) n])
                isPeak = false;
        }

        if (isPeak && flux[(size_t) n] > mean / (float) counted + margin)
            peaks.push_back ({ n, flux[(size_t) n] });
    }

    // Of two closer than the gap, the stronger.
    const auto gapFrames = std::max (1, (int) (onsetGapSeconds * sampleRate / hop));
    std::vector<Peak> kept;

    for (const auto& each : peaks)
    {
        if (! kept.empty() && each.frame - kept.back().frame < gapFrames)
        {
            if (each.flux > kept.back().flux)
                kept.back() = each;

            continue;
        }

        kept.push_back (each);
    }

    auto strongest = 0.0f;

    for (const auto& each : kept)
        strongest = std::max (strongest, each.flux);

    const auto gap = (int) (onsetGapSeconds * sampleRate);
    const auto reach = (int) (0.001 * sampleRate);

    for (const auto& each : kept)
    {
        // Anywhere in the frame that found it, and a hop past it: a hit at the frame's far edge rises there.
        const auto centre = each.frame * hop;
        const auto rise = placeRise (sound, centre - size / 2, centre + size / 2 + hop);
        const auto sample = findCrossingBefore (sound, rise, reach);

        if (sample - onsets.back().sample < gap)
            continue; // the first sound's own, or too near the one before

        onsets.push_back ({ sample, strongest > 0.0f ? each.flux / strongest : 0.0f, 1.0f });
    }

    // From which sensitivity each slices: its place among the others by strength, the strongest first, spread evenly over
    // the sensitivity's range.
    if (const auto hits = (int) onsets.size() - 1; hits > 0)
    {
        std::vector<size_t> order;

        for (size_t i = 1; i < onsets.size(); ++i)
            order.push_back (i);

        std::stable_sort (order.begin(), order.end(), [&onsets] (size_t a, size_t b) { return onsets[a].strength > onsets[b].strength; });

        for (size_t rank = 0; rank < order.size(); ++rank)
            onsets[order[rank]].slicesFrom = ((float) rank + 0.5f) / (float) hits;
    }

    return onsets;
}

int findZeroCrossing (const juce::AudioBuffer<float>& audio, int sample, int reach)
{
    const auto frames = audio.getNumSamples();

    if (frames == 0)
        return 0;

    sample = std::clamp (sample, 0, frames - 1);
    const auto from = std::max (0, sample - reach - 1), to = std::min (frames, sample + reach + 1);
    const auto channels = audio.getNumChannels();
    std::vector<float> stretch ((size_t) (to - from), 0.0f);

    for (int channel = 0; channel < channels; ++channel)
    {
        const auto* samples = audio.getReadPointer (channel);

        for (int i = from; i < to; ++i)
            stretch[(size_t) (i - from)] += samples[i];
    }

    return from + findCrossing (stretch, sample - from, reach);
}
} // namespace phazite::devices
