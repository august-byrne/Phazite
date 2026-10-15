// SPDX-License-Identifier: AGPL-3.0-or-later
// The spectrum of what a device plays, for an analyzer such as the EQ's: the newest samples, windowed and transformed,
// as levels in decibels that rise at once and fall slowly, as analyzers show them. For the message thread: it
// allocates only when made, and takes its samples from a SampleTap.
#pragma once

#include <juce_dsp/juce_dsp.h>

#include <vector>

namespace phazite::devices
{
class Spectrum
{
public:
    // Of the newest 2^order samples (4096 by default: bins about 12 Hz apart at 48 kHz).
    explicit Spectrum (int order = 12);

    void setSampleRate (double newSampleRate) noexcept { sampleRate = newSampleRate; }
    int getSize() const noexcept { return size; }

    // Adds samples, the newest last; only the newest getSize() count.
    void add (const float* samples, int numSamples);

    // Analyzes the newest samples. Each level falls by fallDecibels at most since the last update, so peaks linger.
    void update (float fallDecibels);

    // The level in decibels at a frequency, between the bins around it; a full-scale sine is 0 dB, silence floorDecibels.
    float getLevelAt (double frequency) const;

    static constexpr float floorDecibels = -120.0f;

private:
    const int size;
    juce::dsp::FFT fft;
    std::vector<float> window, history, work, levels;
    int writeIndex = 0;
    double sampleRate = 48000.0;
};
} // namespace phazite::devices
