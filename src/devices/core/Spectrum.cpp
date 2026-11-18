// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Spectrum.h"

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
namespace
{
    constexpr double lowsRate = 12000.0; // about what the lows are analyzed at
    constexpr int lowsOrder = 11;        // their frames' size, 2048 samples: about 170 ms there
    constexpr int lowsPadding = 1;       // zero-padded to twice that
    constexpr int lowsHopsPerFrame = 8;  // a frame every 256 samples there, about 47 a second, more often than a refresh
    constexpr int highsHopsPerFrame = 4; // a frame every quarter of one, about 94 a second
} // namespace

//==============================================================================
void Spectrum::Analysis::prepare (int frameOrder, int paddingOrder, int hopsPerFrame)
{
    frameSize = 1 << frameOrder;
    hop = frameSize / hopsPerFrame;
    fft = std::make_unique<juce::dsp::FFT> (frameOrder + paddingOrder);
    window.assign ((size_t) frameSize, 0.0f);
    history.assign ((size_t) frameSize, 0.0f);
    work.assign ((size_t) (2 * fft->getSize()), 0.0f);
    levels.assign ((size_t) (fft->getSize() / 2 + 1), floorDecibels);
    peaks.assign (levels.size(), floorDecibels);
    writeIndex = 0;
    sinceFrame = 0;
    analyzed = false;
    moving = true;

    // A Hann window, scaled by 2 to make up for its average of one half, so that a sine's peak reads at its level.
    for (int i = 0; i < frameSize; ++i)
        window[(size_t) i] = 2.0f * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) frameSize));
}

void Spectrum::Analysis::clear()
{
    std::fill (history.begin(), history.end(), 0.0f);
    std::fill (levels.begin(), levels.end(), floorDecibels);
    std::fill (peaks.begin(), peaks.end(), floorDecibels);
    writeIndex = 0;
    sinceFrame = 0;
    analyzed = false;
    moving = true; // the next update shows it at rest, once
}

void Spectrum::Analysis::add (float sample)
{
    history[(size_t) writeIndex] = sample;

    if (++writeIndex == frameSize)
        writeIndex = 0;

    if (++sinceFrame == hop)
    {
        sinceFrame = 0;
        analyze();
    }
}

void Spectrum::Analysis::analyze()
{
    // The newest frame, oldest sample first, windowed, then zeros up to the transform's size.
    for (int i = 0; i < frameSize; ++i)
        work[(size_t) i] = history[(size_t) ((writeIndex + i) % frameSize)] * window[(size_t) i];

    std::fill (work.begin() + frameSize, work.end(), 0.0f);
    fft->performFrequencyOnlyForwardTransform (work.data(), true);

    // Magnitudes to decibels: a sine of amplitude a gives a / 2 * frameSize, however far the frame is padded. Each level keeps
    // the loudest frame's.
    const auto scale = 2.0f / (float) frameSize;

    for (size_t i = 0; i < peaks.size(); ++i)
    {
        const auto magnitude = work[i] * scale;
        const auto level = magnitude > 1.0e-6f ? 20.0f * std::log10 (magnitude) : floorDecibels;
        peaks[i] = std::max (peaks[i], level);
    }

    analyzed = true;
}

bool Spectrum::Analysis::update (float fallDecibels)
{
    // Without frames since an update that moved no level, the levels are at rest, at the frames' own or the floor.
    if (! analyzed && ! moving)
        return false;

    analyzed = false;
    moving = false;

    for (size_t i = 0; i < levels.size(); ++i)
    {
        const auto next = std::max (peaks[i], std::max (floorDecibels, levels[i] - fallDecibels));
        moving = moving || ! juce::exactlyEqual (next, levels[i]);
        levels[i] = next;
        peaks[i] = floorDecibels;
    }

    return moving;
}

float Spectrum::Analysis::getLevelAt (double frequency) const
{
    const auto position = frequency / getSpacing();

    if (position <= 0.0)
        return levels.front();

    const auto index = (size_t) position;

    if (index + 1 >= levels.size())
        return levels.back();

    const auto fraction = (float) (position - (double) index);
    return levels[index] + fraction * (levels[index + 1] - levels[index]);
}

float Spectrum::Analysis::getLevelOver (double from, double to) const
{
    const auto spacing = getSpacing();
    const auto first = (int) std::ceil (std::min (from, to) / spacing);
    const auto last = (int) std::floor (std::max (from, to) / spacing);

    if (last < first || first < 0 || first >= (int) levels.size())
        return getLevelAt (0.5 * (from + to));

    auto loudest = floorDecibels;

    for (auto i = first; i <= std::min (last, (int) levels.size() - 1); ++i)
        loudest = std::max (loudest, levels[(size_t) i]);

    return loudest;
}

//==============================================================================
Spectrum::Spectrum()
{
    setSampleRate (48000.0);
}

void Spectrum::setSampleRate (double newSampleRate)
{
    // Each resolution made again only when its frames' size changes; its rate, at once.
    if (const auto order = newSampleRate > 50000.0 ? 12 : 11; highs.fft == nullptr || highs.frameSize != 1 << order)
        highs.prepare (order, 0, highsHopsPerFrame);

    if (const auto taken = std::max (1, (int) std::lround (newSampleRate / lowsRate)); taken != decimation)
    {
        decimation = taken;
        lows.prepare (lowsOrder, lowsPadding, lowsHopsPerFrame);

        // Two moving sums of decimation samples, one after the other: a triangle 2 * decimation - 1 samples long, summing to
        // 1, whose zeros at the new rate and its multiples take out what would fold down onto the lows.
        taps.assign ((size_t) (2 * decimation - 1), 0.0f);

        for (int i = 0; i < (int) taps.size(); ++i)
            taps[(size_t) i] = (float) std::min (i + 1, 2 * decimation - 1 - i) / (float) (decimation * decimation);

        recent.assign (taps.size(), 0.0f);
        newest = 0;
        sinceTaken = 0;
    }

    highs.rate = newSampleRate;
    lows.rate = newSampleRate / (double) decimation;
}

double Spectrum::getLowsSpacing() const noexcept
{
    return lows.getSpacing();
}

void Spectrum::clear()
{
    highs.clear();
    lows.clear();
    std::fill (recent.begin(), recent.end(), 0.0f);
    newest = 0;
    sinceTaken = 0;
}

void Spectrum::add (const float* samples, int numSamples)
{
    const auto length = (int) recent.size();

    for (int i = 0; i < numSamples; ++i)
    {
        const auto sample = samples[i];
        highs.add (sample);

        // The lows' sound: one sample in every decimation, the newest ones weighted by the triangle.
        recent[(size_t) newest] = sample;

        if (++sinceTaken == decimation)
        {
            sinceTaken = 0;
            auto taken = 0.0f;

            for (int k = 0, at = newest; k < length; ++k, at = (at == 0 ? length - 1 : at - 1))
                taken += taps[(size_t) k] * recent[(size_t) at];

            lows.add (taken);
        }

        if (++newest == length)
            newest = 0;
    }
}

bool Spectrum::update (float fallDecibels)
{
    const auto highsMoved = highs.update (fallDecibels);
    const auto lowsMoved = lows.update (fallDecibels);
    return highsMoved || lowsMoved;
}

float Spectrum::getLowsShare (double frequency) const
{
    if (frequency <= lowsTo)
        return 1.0f;

    if (frequency >= highsFrom)
        return 0.0f;

    return 1.0f - (float) (std::log2 (frequency / lowsTo) / std::log2 (highsFrom / lowsTo));
}

float Spectrum::getLevelAt (double frequency) const
{
    const auto share = getLowsShare (frequency);

    if (share >= 1.0f)
        return lows.getLevelAt (frequency);

    if (share <= 0.0f)
        return highs.getLevelAt (frequency);

    return share * lows.getLevelAt (frequency) + (1.0f - share) * highs.getLevelAt (frequency);
}

float Spectrum::getLevelOver (double from, double to) const
{
    // Blended by the share at the middle of what it spans.
    const auto share = getLowsShare (std::sqrt (std::max (1.0e-3, from) * std::max (1.0e-3, to)));

    if (share >= 1.0f)
        return lows.getLevelOver (from, to);

    if (share <= 0.0f)
        return highs.getLevelOver (from, to);

    return share * lows.getLevelOver (from, to) + (1.0f - share) * highs.getLevelOver (from, to);
}
} // namespace phazite::devices
