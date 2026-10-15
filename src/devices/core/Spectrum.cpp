// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Spectrum.h"

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
Spectrum::Spectrum (int order)
    : size (1 << order),
      fft (order),
      window ((size_t) size),
      history ((size_t) size, 0.0f),
      work ((size_t) (2 * size), 0.0f),
      levels ((size_t) (size / 2 + 1), floorDecibels)
{
    // A Hann window, scaled by 2 to make up for its average of one half, so that a sine's peak reads at its level.
    for (int i = 0; i < size; ++i)
        window[(size_t) i] = 2.0f * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size));
}

void Spectrum::add (const float* samples, int numSamples)
{
    for (int i = std::max (0, numSamples - size); i < numSamples; ++i)
    {
        history[(size_t) writeIndex] = samples[i];
        writeIndex = (writeIndex + 1) % size;
    }
}

void Spectrum::update (float fallDecibels)
{
    // The newest samples, oldest first, windowed.
    for (int i = 0; i < size; ++i)
        work[(size_t) i] = history[(size_t) ((writeIndex + i) % size)] * window[(size_t) i];

    std::fill (work.begin() + size, work.end(), 0.0f);
    fft.performFrequencyOnlyForwardTransform (work.data(), true);

    // Magnitudes to decibels: a sine of amplitude a gives a / 2 * size before the scaling.
    const auto scale = 2.0f / (float) size;

    for (size_t bin = 0; bin < levels.size(); ++bin)
    {
        const auto magnitude = work[bin] * scale;
        const auto level = magnitude > 1.0e-6f ? 20.0f * std::log10 (magnitude) : floorDecibels;
        levels[bin] = std::max (level, std::max (floorDecibels, levels[bin] - fallDecibels));
    }
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
} // namespace phazite::devices
