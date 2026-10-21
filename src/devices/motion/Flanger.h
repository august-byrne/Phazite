// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's flanger (roadmap step 2.17): the sound mixed with a copy of itself delayed by a few milliseconds, the delay swept
// by an LFO, so that a comb of notches slides through it, the jet-plane whoosh of tape flanging. Delay is the longest the
// copy lags (3 ms at first), and the LFO shortens it by up to five octaves as Depth says, in time with the song (two bars at
// first) or in hertz, as a triangle or a sine, the right side a part of a cycle ahead (Stereo). Feedback rings the comb
// (metallic), or with its sign turned hollows it, with its notches as deep at any feedback; its sign glides through nothing,
// so that a turn across it doesn't click. Its level stays about the same at any feedback. Safe Bass leaves what is below a
// frequency as it was. No latency; no through-zero flanging, which would need it.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/LiveValue.h"
#include "devices/core/Processor.h"
#include "devices/dsp/CombFilter.h"
#include "devices/dsp/Filters.h"
#include "devices/dsp/SyncedLfo.h"

#include <array>
#include <atomic>
#include <span>

namespace phazite::devices
{
class Flanger final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteFlanger";

    enum Parameter
    {
        delay, // milliseconds: the longest the copy lags
        depth,
        sync,
        division, // a note value (devices/dsp/NoteValues.h)
        rate,
        shape, // sine or triangle
        feedback,
        stereo,
        safeBass,
        mix // applied by its thin plug-in (DeviceInfo.h, mixParameterID)
    };

    static const DeviceInfo info;

    // How far the copy lags, in milliseconds, for the parameters' values and the LFO's value (-1 at the bottom of its sweep,
    // the shortest, to 1 at the top, the Delay itself): for its editor and tests.
    static double getSweptDelay (std::span<const float> parameters, float lfoValue);

    // Its response at a frequency with the copy lagging so many milliseconds, as a gain: for its editor and tests.
    static double getMagnitude (std::span<const float> parameters, double milliseconds, double frequency, double sampleRate);

    // How far the copy lags each side now, in milliseconds, and the LFO's phase (0 to 1), once a block
    // (devices/core/LiveValue.h).
    const LiveValue& getDelay (int channel) const noexcept { return delays[(size_t) channel]; }
    const LiveValue& getPhase() const noexcept { return shownPhase; }

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    double getTailSeconds() const override { return tailSeconds.load (std::memory_order_relaxed); }

    static constexpr float sweepOctaves = 5.0f; // how much shorter the copy lags at the bottom of a full sweep

private:
    static constexpr int controlStep = 8; // samples between new delays

    double sampleRate = 44100.0;
    float signStep = 0.1f; // how far the feedback's sign moves per control step while it turns
    float sign = 1.0f;     // the feedback's sign, gliding through 0 when it turns
    dsp::SyncedLfo lfo;
    std::array<dsp::CombFilter, 2> combs;
    std::array<dsp::Filter, 2> lowParts {}; // Safe Bass's: what is kept as it was
    std::atomic<double> tailSeconds { 0.05 };
    std::array<LiveValue, 2> delays;
    LiveValue shownPhase;

    static_assert (std::atomic<double>::is_always_lock_free);
};
} // namespace phazite::devices
