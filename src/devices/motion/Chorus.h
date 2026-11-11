// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's chorus: the sound with copies of itself a few milliseconds behind, each copy's delay swept by an LFO so that its
// pitch wavers a little, as several players never quite in tune with each other sound. A copy each side at first, the right
// side's LFO a part of a cycle ahead (Stereo; the sides opposite at first), as the Juno-60's chorus plays (its mode I at
// first: the copy's delay swept from 1.75 to 5.25 ms by a triangle at 0.5 Hz). Voices gives each side up to eight copies, as
// the multi-voice choruses do (Calf's MultiChorus, Logic's Ensemble), together as loud as one. Voice Phase is how far apart
// in the LFO's cycle a side's copies are: at 100 % so far that the copies of both sides, the sides opposite, are spread
// evenly round it (three a side a third of a cycle apart, as the string machines' ensembles sweep their three lines), which
// sounds the steadiest; closer, they swirl together. Spread gives each copy a part of the sweep to itself, the first the
// shortest delays and the last the longest: at 100 % the parts lie end to end, at none every copy sweeps the whole of it
// (MultiChorus's Overlap, the other way round). Shimmer puts a sine ten times as fast and a tenth as deep on the sweep, as
// the string machines' second LFO does: three copies with it are their ensemble, thicker and smoother. Delay is the middle
// of the sweep and Depth how far it goes each way, as a part of the delay; the LFO in hertz (0.5 Hz at first) or in time
// with the song, a triangle or a sine, as the phaser's and the flanger's (devices/dsp/SyncedLfo.h). Feedback sends the
// copies back in, toward a flanger's ring, its peaks rising by as many decibels as what lies between them falls. High Cut
// darkens the copies and what they feed back, as a bucket-brigade line does; Safe Bass leaves what is below a frequency as
// it was, the copies hearing only what is above it, 24 dB an octave down below it (SafeBass.h), so that a bass's lows stay
// in the middle and still. The mix is half and half at first, the sound and its copies as loud; more of it makes the
// copies the louder (at two thirds, twice as loud as the sound), and at full Mix only the copies play, which with one copy
// is a vibrato. No latency.
#pragma once

#include "SafeBass.h"
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
        voices, // how many copies a side: 1 to 8
        delay,  // milliseconds: the middle of the sweep
        depth,  // how far the sweep goes each way, as a part of the delay
        sync,
        division, // a note value (devices/dsp/NoteValues.h)
        rate,
        shape,      // sine or triangle
        shimmer,    // how much of the fast sine is on the sweep
        voicePhase, // how far apart in the LFO's cycle a side's copies are, as a part of an even spread
        spread,     // how much of the sweep each copy has to itself
        feedback,
        stereo,
        highCut,
        safeBass,
        mix // applied by its runner (DeviceInfo.h, mixParameterID)
    };

    static const DeviceInfo info;

    static constexpr int mostVoices = 8; // a side's copies, at most

    // How many copies each side has.
    static int getNumVoices (std::span<const float> parameters);

    // Where in the LFO's cycle a side's copy is, given where the LFO is (0 to 1): the right side a part of a cycle ahead,
    // and each copy a part of a cycle after the one before (Voice Phase: with three copies, up to a third).
    static double getVoicePhase (std::span<const float> parameters, double lfoPhase, int channel, int voice);

    // The sweep at a point in its cycle, from -1 (the bottom of a copy's sweep) to 1 (its top): the LFO's shape, with the
    // fast sine on it as Shimmer says. And how far a copy there lags, in milliseconds: within the whole sweep, or within
    // its own part of it (Spread). For its editor and tests.
    static float getSweep (std::span<const float> parameters, double phase);
    static double getVoiceDelay (std::span<const float> parameters, double phase, int voice);

    // The shortest and the longest a copy lags over its sweep, in milliseconds: the whole sweep's ends, or with Spread
    // those of its own part of it.
    struct Reach
    {
        double shortest = 0.0, longest = 0.0;
    };

    static Reach getVoiceReach (std::span<const float> parameters, int voice);

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
    static constexpr int numSets = 2;     // the copies that play, and while their number changes, the ones that played

    // A set of copies: so many a side, where the number of voices says they are. When that number changes, the copies that
    // play fade out where they are while a set of the new number fades in, rather than each jumping to its new place (a
    // click), so two sets play for a moment.
    struct Set
    {
        int count = 0;                         // its copies a side; none while it rests
        float fade = 0.0f;                     // from 0 (faded out) to 1
        float outGain = 0.0f, loopGain = 0.0f; // each copy's share of what comes out and of what goes back, now
    };

    // A side: the line its copies read from, where each set's copies are, what they fed back, and High Cut's filter.
    struct Side
    {
        dsp::DelayLine line;
        std::array<std::array<float, mostVoices>, numSets> now {}, step {}; // each copy's delay, in samples, and its glide a sample
        dsp::Filter cut;
    };

    double sampleRate = 44100.0;
    float longestSamples = 2.0f; // the longest delay a line has room for
    float fadeStep = 0.01f;      // how far a set fades a control step
    bool fresh = true;           // the first block after a reset takes its delays and gains at once
    dsp::SyncedLfo lfo;
    std::array<Set, numSets> sets;
    int front = 0; // the set that plays the copies as their number is now
    std::array<Side, 2> sides;
    SafeBass bassSplit;
    std::atomic<double> tailSeconds { 0.02 };
    std::array<std::array<LiveValue, mostVoices>, 2> delays;
    LiveValue shownPhase;

    static_assert (std::atomic<double>::is_always_lock_free);
};
} // namespace phazite::devices
