// SPDX-License-Identifier: AGPL-3.0-or-later
// The true peak of a channel as ITU-R BS.1770-5's Annex 2 finds it: the waveform between the samples, as a converter
// rebuilds it, oversampled four times by the Recommendation's interpolating filter (48 taps: 12 for each of four points
// between two samples). Each sample in brings the four points an eighth, three eighths, five eighths and seven eighths of
// the way from the sample pointAge before it to the next. At 44.1 kHz it reads a sine's peak within 0.25 dB up to 18 kHz.
// The limiter keeps its ceiling by it, and the loudness meter shows it (engine/nodes/LoudnessMeter.h), so they agree.
// Real-time safe.
#pragma once

#include <array>

namespace phazite::devices
{
class TruePeak
{
public:
    static constexpr int taps = 12;    // of each point's filter
    static constexpr int pointAge = 6; // the points a sample brings follow the sample this many before it

    // Forgets the samples it was given.
    void reset() noexcept;

    // Takes the next sample, and returns the loudest of the four points it brings (as a magnitude).
    float push (float sample) noexcept;

    // A sample it was given: 0 is the last, up to taps - 1.
    float getSample (int age) const noexcept { return history[(size_t) (newest - age + taps)]; }

private:
    // The last samples twice over, so that the newest twelve are always in a row: the newest at newest and at newest + taps.
    std::array<float, 2 * taps> history {};
    int newest = taps - 1;
};
} // namespace phazite::devices
