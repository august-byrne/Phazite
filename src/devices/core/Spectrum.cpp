// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Spectrum.h"

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
Spectrum::Spectrum()
{
    prepare (11);
}

void Spectrum::prepare (int newOrder)
{
    order = newOrder;
    size = 1 << order;
    hop = size / 4;
    fft = std::make_unique<juce::dsp::FFT> (order);
    window.assign ((size_t) size, 0.0f);
    history.assign ((size_t) size, 0.0f);
    work.assign ((size_t) (2 * size), 0.0f);
    levels.assign ((size_t) (size / 2 + 1), floorDecibels);
    peaks.assign ((size_t) (size / 2 + 1), floorDecibels);
    writeIndex = 0;
    sinceFrame = 0;
    analyzed = false;
    moving = true;

    // A Hann window, scaled by 2 to make up for its average of one half, so that a sine's peak reads at its level.
    for (int i = 0; i < size; ++i)
        window[(size_t) i] = 2.0f * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size));
}

void Spectrum::setSampleRate (double newSampleRate)
{
    sampleRate = newSampleRate;

    if (const auto wanted = newSampleRate > 50000.0 ? 12 : 11; wanted != order)
        prepare (wanted);
}

void Spectrum::add (const float* samples, int numSamples)
{
    // A frame each hop, every one read however many samples come at once: at 48 kHz, 94 frames a second.
    for (int done = 0; done < numSamples;)
    {
        const auto count = std::min (numSamples - done, hop - sinceFrame);

        for (int i = 0; i < count; ++i)
        {
            history[(size_t) writeIndex] = samples[done + i];
            writeIndex = (writeIndex + 1) % size;
        }

        done += count;
        sinceFrame += count;

        if (sinceFrame == hop)
        {
            sinceFrame = 0;
            analyze();
        }
    }
}

void Spectrum::analyze()
{
    // The newest frame, oldest sample first, windowed.
    for (int i = 0; i < size; ++i)
        work[(size_t) i] = history[(size_t) ((writeIndex + i) % size)] * window[(size_t) i];

    std::fill (work.begin() + size, work.end(), 0.0f);
    fft->performFrequencyOnlyForwardTransform (work.data(), true);

    // Magnitudes to decibels: a sine of amplitude a gives a / 2 * size before the scaling. Each bin keeps the loudest frame's.
    const auto scale = 2.0f / (float) size;

    for (size_t bin = 0; bin < peaks.size(); ++bin)
    {
        const auto magnitude = work[bin] * scale;
        const auto level = magnitude > 1.0e-6f ? 20.0f * std::log10 (magnitude) : floorDecibels;
        peaks[bin] = std::max (peaks[bin], level);
    }

    analyzed = true;
}

bool Spectrum::update (float fallDecibels)
{
    // Without frames since an update that moved no level, the levels are at rest, at the frames' own or the floor.
    if (! analyzed && ! moving)
        return false;

    analyzed = false;
    moving = false;

    for (size_t bin = 0; bin < levels.size(); ++bin)
    {
        const auto next = std::max (peaks[bin], std::max (floorDecibels, levels[bin] - fallDecibels));
        moving = moving || ! juce::exactlyEqual (next, levels[bin]);
        levels[bin] = next;
        peaks[bin] = floorDecibels;
    }

    return moving;
}

float Spectrum::getLevelAt (double frequency) const
{
    const auto position = frequency * size / sampleRate;

    if (position <= 0.0)
        return levels.front();

    const auto bin = (size_t) position;

    if (bin + 1 >= levels.size())
        return levels.back();

    const auto fraction = (float) (position - (double) bin);
    return levels[bin] + fraction * (levels[bin + 1] - levels[bin]);
}

float Spectrum::getLevelOver (double from, double to) const
{
    const auto first = (int) std::ceil (std::min (from, to) * size / sampleRate);
    const auto last = (int) std::floor (std::max (from, to) * size / sampleRate);

    if (last < first || first < 0 || first >= (int) levels.size())
        return getLevelAt (0.5 * (from + to));

    auto loudest = floorDecibels;

    for (auto bin = first; bin <= std::min (last, (int) levels.size() - 1); ++bin)
        loudest = std::max (loudest, levels[(size_t) bin]);

    return loudest;
}
} // namespace phazite::devices
