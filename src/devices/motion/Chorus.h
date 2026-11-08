// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's chorus: the sound with copies of itself a few milliseconds behind, each copy's delay swept by an LFO so that its
// pitch wavers a little, as several players never quite in tune with each other sound. Classic: a copy each side, the right
// side's LFO a part of a cycle ahead (Stereo; the sides opposite at first), as the Juno-60's chorus plays (its mode I at
// first: the copy's delay swept from 1.75 to 5.25 ms by a triangle at 0.5 Hz). Ensemble: three copies each side, their
// LFOs a third of a cycle apart, each with a sine ten times as fast and a tenth as deep on it, as the string machines'
// ensembles sweep theirs (a slow LFO and a fast one, 120 degrees apart): thicker and smoother. Delay is the middle of the
// sweep and Depth how far it goes each way, as a part of the delay; the LFO in hertz (0.5 Hz at first) or in time with the
// song, a triangle or a sine, as the phaser's and the flanger's (devices/dsp/SyncedLfo.h). Feedback sends the copies back
// in, toward a flanger's ring, its peaks rising by as many decibels as what lies between them falls. High Cut darkens the
// copies and what they feed back, as a bucket-brigade line does; Safe Bass leaves what is below a frequency as it was. The
// mix is half and half at first, the sound and its copies as loud; at full Mix only the copies play, which in Classic is a
// vibrato. No latency.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/LiveValue.h"
#include "devices/core/Processor.h"
#include "devices/dsp/DelayLine.h"
#include "devices/dsp/Filters.h"
#include "devices/dsp/SyncedLfo.h"

#include <array>
#include <atomic>
#include <span>

namespace phazite::devices
{
class Chorus final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteChorus";

    enum Parameter
    {
        mode,  // Classic or Ensemble
        delay, // milliseconds: the middle of the sweep
        depth, // how far the sweep goes each way, as a part of the delay
        sync,
        division, // a note value (devices/dsp/NoteValues.h)
        rate,
        shape, // sine or triangle
        feedback,
        stereo,
        highCut,
        safeBass,
        mix // applied by its runner (DeviceInfo.h, mixParameterID)
    };

    static const DeviceInfo info;

    static constexpr int mostVoices = 3; // a side's copies, in Ensemble

    // How many copies each side has: 1 in Classic, 3 in Ensemble.
    static int getNumVoices (std::span<const float> parameters);

    // Where in the LFO's cycle a side's copy is, given where the LFO is (0 to 1): the right side a part of a cycle ahead,
    // and in Ensemble each copy a third of a cycle after the one before.
    static double getVoicePhase (std::span<const float> parameters, double lfoPhase, int channel, int voice);

    // The sweep at a point in its cycle, from -1 (the shortest delay) to 1 (the longest): the LFO's shape, and in Ensemble
    // the fast sine on it. And how far a copy there lags, in milliseconds. For its editor and tests.
    static float getSweep (std::span<const float> parameters, double phase);
    static double getVoiceDelay (std::span<const float> parameters, double phase);

    // Its response at a frequency, as a gain, with a side's copies lagging so many milliseconds each (as many as it has) and
    // the sound mixed in as its mix says: for its editor and tests.
    static double getMagnitude (std::span<const float> parameters, std::span<const double> milliseconds, double frequency,
                                double sampleRate);

    // How far each side's copies lag now, in milliseconds, and the LFO's phase (0 to 1), once a block
    // (devices/core/LiveValue.h).
    const LiveValue& getDelay (int channel, int voice) const noexcept { return delays[(size_t) channel][(size_t) voice]; }
    const LiveValue& getPhase() const noexcept { return shownPhase; }

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    double getTailSeconds() const override { return tailSeconds.load (std::memory_order_relaxed); }

private:
    static constexpr int controlStep = 8; // samples between new delays

    // A side: the line its copies read from, where each copy is and how loud it plays (gliding when the mode changes, so
    // that a copy fades in or out rather than clicking), what they fed back, and its filters.
    struct Side
    {
        dsp::DelayLine line;
        std::array<float, mostVoices> now {}, step {};         // each copy's delay, in samples, and its glide a sample
        std::array<float, mostVoices> outGain {}, loopGain {}; // each copy's share of what comes out and of what goes back
        dsp::Filter cut, low;                                  // High Cut's, and Safe Bass's: what is kept as it was
    };

    double sampleRate = 44100.0;
    float longestSamples = 2.0f; // the longest delay a line has room for
    float gainStep = 0.01f;      // how far a copy's gain glides a sample, when the mode changes
    bool fresh = true;           // the first block after a reset takes its delays and gains at once
    dsp::SyncedLfo lfo;
    std::array<Side, 2> sides;
    std::atomic<double> tailSeconds { 0.02 };
    std::array<std::array<LiveValue, mostVoices>, 2> delays;
    LiveValue shownPhase;

    static_assert (std::atomic<double>::is_always_lock_free);
};
} // namespace phazite::devices
