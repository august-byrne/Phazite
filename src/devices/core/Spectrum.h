// SPDX-License-Identifier: AGPL-3.0-or-later
// The spectrum of what a device plays, for an analyzer such as the EQ's: frames of its samples as they come, windowed and
// transformed, as levels in decibels. Each level shows the loudest any frame gave it since it was last shown, rising at once
// and falling at a steady rate, as FabFilter's Pro-Q and Voxengo SPAN show short sounds. Read once a refresh from the newest
// samples alone, as it was, a clap between two refreshes read 6 to 20 dB low or not at all, and every frame between them was
// thrown away (the owner, 2026-11-10: the EQ didn't show short sounds such as claps).
//
// In two resolutions, as Smaart's multi-time window gives the lows long frames and the highs short ones, and as Pro-Q's and
// Logic Pro's higher resolutions give the lows their detail (the owner, 2026-11-13: a sub's sine showed as a block 50 Hz
// wide; docs/design.md, section 9):
// - the highs, from highsFrom up, in frames of about 43 ms, each a quarter of a frame after the last, their levels about
//   23 Hz apart, so that a short sound fills enough of one to read near its level;
// - the lows, up to lowsTo, in frames of about 170 ms of the sound at about 12 kHz (taken down to it by two moving sums,
//   which leave out what would fold down into the lows), zero-padded to twice their length, so that their levels lie about
//   3 Hz apart and a sine shows as a narrow peak where it is, each frame an eighth of one after the last, more often than
//   the display refreshes. Small transforms of a slower sound, as Smaart's are: they cost about as much as the highs';
// - between them, an octave over which the lows' share falls as the frequency rises.
// Both read a steady sine at its level. What is spread over many frequencies, as noise is, reads lower in the lows' narrower
// levels (6 dB for noise), and a short sound in the lows reads lower and later in their longer frames, as in Pro-Q's
// highest resolution. For the message thread: it allocates only when made and when a new sample rate changes its frames'
// sizes, and takes its samples from a SampleTap.
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

    // The highs' frames: 2048 samples up to 48 kHz, 4096 above. The lows': 2048 samples of the sound at about 12 kHz.
    void setSampleRate (double);
    int getSize() const noexcept { return highs.frameSize; }
    double getLowsSpacing() const noexcept; // in hertz, how far apart the lows' levels are: about 3

    // Adds samples, the newest last, analyzing each resolution's frame as its samples come.
    void add (const float* samples, int numSamples);

    // Afresh: what it has heard goes, and every level is at the floor, so that the next update shows what comes from now on
    // (a display shown again, whose sound went on while nobody looked). Allocates nothing.
    void clear();

    // Shows the frames analyzed since the last update: each level rises to the loudest of them, or falls by fallDecibels at
    // most. Returns whether any level moved; at rest, with nothing added, false.
    bool update (float fallDecibels);

    // The level in decibels at a frequency, between the levels around it; a full-scale sine is 0 dB, silence floorDecibels.
    float getLevelAt (double frequency) const;

    // The loudest level between two frequencies: what a pixel that spans many of them shows, so that a peak between the
    // frequencies drawn isn't missed. Narrower than the levels' spacing, the level at their middle.
    float getLevelOver (double from, double to) const;

    static constexpr float floorDecibels = -120.0f;
    static constexpr double lowsTo = 150.0, highsFrom = 300.0; // the lows alone below the one, the highs above the other

private:
    // One resolution: frames of so many samples of what it is given, at that rate, windowed and zero-padded to the
    // transform's size, one every hop samples.
    struct Analysis
    {
        int frameSize = 0, hop = 0;
        double rate = 48000.0;
        std::unique_ptr<juce::dsp::FFT> fft;
        std::vector<float> window, history, work, levels, peaks;
        int writeIndex = 0, sinceFrame = 0;
        bool analyzed = false, moving = true; // frames analyzed since the last update, and levels it moved

        void prepare (int frameOrder, int paddingOrder, int hopsPerFrame);
        void clear();
        void add (float sample);
        void analyze();
        bool update (float fallDecibels);
        double getSpacing() const noexcept { return rate / (double) fft->getSize(); }
        float getLevelAt (double frequency) const;
        float getLevelOver (double from, double to) const;
    };

    Analysis highs, lows;

    // The lows' sound: every so many samples, the newest ones through two moving sums as long (weighted as a triangle), which
    // leave out what would fold down into the lows as the rate is divided by as much.
    int decimation = 0, sinceTaken = 0, newest = 0;
    std::vector<float> recent, taps;

    float getLowsShare (double frequency) const; // 1 up to lowsTo, 0 from highsFrom
};
} // namespace phazite::devices
