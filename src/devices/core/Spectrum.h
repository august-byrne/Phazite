// SPDX-License-Identifier: AGPL-3.0-or-later
// The spectrum of what a device plays, for an analyzer such as the EQ's: frames of its samples as they come, each a quarter
// of a frame after the last, windowed and transformed, as levels in decibels. Each level shows the loudest any frame gave it
// since it was last shown, rising at once and falling at a steady rate, as FabFilter's Pro-Q and Voxengo SPAN show short
// sounds. Read once a refresh from the newest samples alone, as it was, a clap between two refreshes read 6 to 20 dB low or
// not at all, and every frame between them was thrown away (the owner, 2026-11-10: the EQ didn't show short sounds such as
// claps). For the message thread: it allocates only when made and when a new sample rate changes its frames' size, and
// takes its samples from a SampleTap.
#pragma once

#include <juce_dsp/juce_dsp.h>

#include <memory>
#include <vector>

namespace phazite::devices
{
class Spectrum
{
public:
    Spectrum();

    // Frames of about 43 ms: 2048 samples up to 48 kHz (bins about 23 Hz apart there), 4096 above, so that a short sound
    // fills enough of one to read near its level. A longer frame reads a clap lower, averaging it with the quiet around it.
    void setSampleRate (double);
    int getSize() const noexcept { return size; }

    // Adds samples, the newest last, and analyzes a frame each time a quarter of a frame of them has come.
    void add (const float* samples, int numSamples);

    // Shows the frames analyzed since the last update: each level rises to the loudest of them, or falls by fallDecibels at
    // most. Returns whether any level moved; at rest, with nothing added, false.
    bool update (float fallDecibels);

    // The level in decibels at a frequency, between the bins around it; a full-scale sine is 0 dB, silence floorDecibels.
    float getLevelAt (double frequency) const;

    // The loudest level between two frequencies: what a pixel that spans many bins shows, so that a peak between the
    // frequencies drawn isn't missed. Narrower than a bin, the level at their middle.
    float getLevelOver (double from, double to) const;

    static constexpr float floorDecibels = -120.0f;

private:
    int order = 11, size = 1 << 11, hop = (1 << 11) / 4;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, history, work, levels, peaks;
    int writeIndex = 0, sinceFrame = 0;
    double sampleRate = 48000.0;
    bool analyzed = false, moving = true; // frames analyzed since the last update, and levels it moved

    void prepare (int newOrder);
    void analyze();
};
} // namespace phazite::devices
