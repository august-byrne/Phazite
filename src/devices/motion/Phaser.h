// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's phaser (roadmap step 2.17): the sound mixed half and half with itself through all-pass filters whose phase an
// LFO sweeps, so that notches move up and down its spectrum, as the classic four-stage phasers do. Notches sets how many (two
// all-pass stages each; 2, the classic, at first); the LFO sweeps them around a frequency, up to two octaves each way as
// Depth says, in time with the song (a bar at first) or in hertz, as a sine or a triangle, the right side a part of a cycle
// ahead (Stereo). Feedback makes it ring, as a filter's resonance does: above 0, at the peaks between the notches; below 0,
// at the notches themselves, which fill in and then turn into peaks. It is solved within the sample, so it never runs away.
// Spacing spreads the notches apart or draws them together; Safe Bass leaves what is below a frequency as it was. No
// latency.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/LiveValue.h"
#include "devices/core/Processor.h"
#include "devices/dsp/Filters.h"
#include "devices/dsp/SyncedLfo.h"

#include <array>
#include <atomic>
#include <span>

namespace phazite::devices
{
class Phaser final : public Processor
{
public:
    static constexpr const char* typeID = "phazitePhaser";
    static constexpr int maxNotches = 6;

    enum Parameter
    {
        notches, // a choice: 1 to 6
        frequency,
        depth,
        sync,
        division, // a note value (devices/dsp/NoteValues.h)
        rate,
        shape, // sine or triangle
        feedback,
        spacing,
        stereo,
        safeBass,
        mix // applied by its thin plug-in (DeviceInfo.h, mixParameterID)
    };

    static const DeviceInfo info;

    // Where its notches are swept to, in hertz, for the parameters' values and the LFO's value (-1 at the bottom of its sweep
    // to 1 at the top): for its editor and tests.
    static double getSweptCentre (std::span<const float> parameters, float lfoValue, double sampleRate);

    // Its response at a frequency for the parameters' values with its notches swept to centre (in hertz), as a gain: for its
    // editor and tests.
    static double getMagnitude (std::span<const float> parameters, double centre, double frequency, double sampleRate);

    // Where its sweep is, once a block (devices/core/LiveValue.h): each side's centre, in hertz, and the LFO's phase (0 to 1).
    const LiveValue& getCentre (int channel) const noexcept { return centres[(size_t) channel]; }
    const LiveValue& getPhase() const noexcept { return shownPhase; }

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    double getTailSeconds() const override;

private:
    static constexpr int controlStep = 8; // samples between new coefficients

    double sampleRate = 44100.0;
    dsp::SyncedLfo lfo;
    std::array<std::array<dsp::Filter, maxNotches>, 2> sections {};
    std::array<dsp::Filter, 2> lowParts {};    // Safe Bass's: what is kept as it was
    std::atomic<float> shownFeedback { 0.0f }; // for its tail, which the message thread asks for
    std::array<LiveValue, 2> centres;
    LiveValue shownPhase;
};
} // namespace phazite::devices
