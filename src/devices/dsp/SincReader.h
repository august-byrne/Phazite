// SPDX-License-Identifier: AGPL-3.0-or-later
// A sound read between its samples by a windowed sinc whose cutoff follows how fast it is read, for the drum sampler's
// pads, tuned or at another rate than the engine's: 16 zero crossings on either side, Kaiser-windowed (about 70 dB down
// past its band), so that the top octave keeps its level (flat to about 0.43 of the sound's rate), and a sound read
// faster than its rate (pitched up) has what would fold back as aliasing taken away first, its kernel stretched to as
// many samples as it steps over (up to 8 times as many). Real-time safe once the kernel is made: the first reader makes
// it, off the audio thread.
#pragma once

#include <vector>

namespace phazite::devices::dsp
{
class SincReader
{
public:
    static constexpr int zeroCrossings = 16;
    static constexpr int resolution = 512; // kernel points between two zero crossings

    SincReader();

    // A sound's one or two channels (the second the first for a mono sound), `length` samples long, at a position in its
    // samples, read `step` samples apart: silence beyond its ends.
    void read (const float* first, const float* second, int length, double position, double step, float& left, float& right) const noexcept;

    // The same for one channel, at half the cost of reading it as both.
    float read (const float* samples, int length, double position, double step) const noexcept;

    // A channel at half its rate, (length + 1) / 2 samples into halved, the first where the channel's first is: what would
    // fold back as aliasing taken away first, by the kernel at half its band, which is zero at every whole sample but its
    // middle (a half-band filter), so that a sound read an octave or more faster than its rate can be read from it, stepping
    // over half as many samples. Not on the audio thread.
    static void halve (const float* samples, int length, float* halved);

private:
    const std::vector<float>* kernel = nullptr; // the windowed sinc from 0 to zeroCrossings, every 1/resolution
};
} // namespace phazite::devices::dsp
