// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's EQ (roadmap steps 1.6 and S6.4): eight bands, each a low cut, low shelf, bell, notch, high shelf or high cut,
// then an output gain. A new EQ changes nothing: its cuts are off and its other bands at 0 dB. Each band plays on the whole
// sound, its middle, its sides, its left or its right (its placement), and a bell or a shelf can be dynamic: its gain moves
// with the level of the sound in its band, as a de-esser's does. Its parameters move smoothly, so turning a knob doesn't
// click, and it taps what it plays for its editor's analyzer. Its bands are matched to their analog shapes
// (dsp::makeMatchedFilter()), so a bell or a shelf near the top keeps its width and symmetry. Its Phase plays the bands as
// they are (Minimum), or the same curve with every frequency as late as every other (Linear: LinearPhase.h), at a latency.
#pragma once

#include "LinearPhase.h"
#include "devices/core/DeviceInfo.h"
#include "devices/core/LiveValue.h"
#include "devices/core/Processor.h"
#include "devices/core/SampleTap.h"
#include "devices/dsp/ControlGrid.h"
#include "devices/dsp/Filters.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

namespace phazite::devices
{
class Equalizer final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteEq";
    static constexpr int numBands = 8;

    enum class BandType
    {
        lowCut,
        lowShelf,
        bell,
        notch,
        highShelf,
        highCut
    };

    // What of the sound a band plays on: both sides alike, the middle (L + R) / 2 or the side (L - R) / 2 alone (the other
    // left as it is), or one side alone. On a mono channel the stereo, middle and left bands play on it, the others on nothing.
    enum class Placement
    {
        stereo,
        mid,
        side,
        left,
        right
    };

    // A band's parameters, in this order: band b's are from b * fieldsPerBand on. The output gain is last.
    enum Field
    {
        on,
        type,
        frequency,
        gain,
        q,
        slope,     // a cut's steepness: 12, 24, 36 or 48 dB per octave
        placement, // Placement
        dynamic,   // how far a bell's or a shelf's gain moves beyond its Gain as its band gets loud, in decibels; 0: it doesn't
        threshold  // the level of the sound in its band from which it moves, in decibels
    };

    // How it plays its bands: as they are, each turning the phase as its analog shape does, or with every frequency as late as
    // every other (LinearPhase.h), the same curve at a latency.
    enum class Phase
    {
        minimum,
        linear
    };

    static constexpr int fieldsPerBand = 9;
    static constexpr int outputParameter = numBands * fieldsPerBand;
    static constexpr int phaseParameter = outputParameter + 1; // Phase
    static constexpr int mixParameter = outputParameter + 2;   // applied by its runner (DeviceInfo.h, mixParameterID)
    static constexpr int getParameterIndex (int band, Field field) noexcept { return band * fieldsPerBand + (int) field; }

    // How far above its Threshold the sound in a dynamic band must be for its gain to move by the whole Dynamic: from none
    // at the Threshold to all of it this many decibels above.
    static constexpr float dynamicRange = 12.0f;

    // The level a dynamic band hears: the loudest of its sound over the last 25 ms (counted in whole milliseconds, so 24 to 25
    // of them: longer than half a cycle of a 30 Hz bass, so that it doesn't fall between a low tone's peaks), followed up 63 %
    // of the way in 3 ms and down 63 % of the way in 60 ms. A level that has fallen this far under the band's Threshold, and
    // whose loudest is too, is none, so that what comes next finds it as it would after any silence however long.
    static constexpr int earHoldMilliseconds = 25;
    static constexpr double earRiseSeconds = 0.003, earFallSeconds = 0.06;
    static constexpr float earFloorUnderThreshold = 24.0f;

    // The loudest a dynamic band is expected to hear, in decibels over full scale, from which its tail counts the fall to its
    // floor (getTailSeconds()).
    static constexpr float earHeadroom = 12.0f;

    static const DeviceInfo info;

    // The EQ's gain (not in decibels) at a frequency for the parameters' values given in the device's order and ranges,
    // as it plays them once settled, with each band at its Gain (a dynamic band's gain as it plays now is the caller's to put
    // there): for its editor's curves and for tests. getBandMagnitude() gives one band's. Each is for one way through the EQ:
    // what acts on the middle (the bands on Stereo and those on Mid), on the side (on Stereo and on Side), on the left (on
    // Stereo and on Left) or on the right (on Stereo and on Right); or, for Stereo, the bands on Stereo alone, which act on all
    // of it. A Left or a Right band acts on half of the middle and of the side and mixes them, which no single curve shows: the
    // middle's and the side's leave it out, as the left's and the right's leave out the Mid and Side bands.
    static double getMagnitude (std::span<const float> parameters, double frequency, double sampleRate, Placement way = Placement::stereo);
    static double getBandMagnitude (std::span<const float> parameters, int band, double frequency, double sampleRate,
                                    Placement way = Placement::stereo);

    // Whether a band placed so acts on a way through the EQ, as getMagnitude() counts it.
    static constexpr bool actsOn (Placement band, Placement way) noexcept { return band == Placement::stereo || band == way; }

    // A band's filter sections as getBandMagnitude() works them out, designed once, for a curve of many points: its gain (not in
    // decibels) at a frequency. None for a band that is off.
    static constexpr int maxSections = 4; // a 48 dB per octave cut

    struct BandShape
    {
        int numSections = 0;
        std::array<dsp::FilterCoefficients, maxSections> sections {};

        double getMagnitude (double frequency, double sampleRate) const;
    };

    static BandShape getBandShape (std::span<const float> parameters, int band, double sampleRate);

    // What it played, for its editor's analyzer (devices/core/SampleTap.h), and the rate it plays at (message thread).
    SampleTap& getOutputTap() noexcept { return outputTap; }
    double getSampleRate() const noexcept { return sampleRate; }

    // How far a band's Dynamic moves its gain now, in decibels, beyond its Gain: set once a block, for its editor (any thread),
    // which shows the band's gain as it plays now as its Gain and this. While the EQ doesn't play (asleep on silence), the
    // move it made last, which has fallen back by then.
    float getGainMoved (int band) const noexcept { return gainsMoved[(size_t) band].get(); }

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;

    // While any band is dynamic, as long as its level takes to fall from earHeadroom over full scale to the floor under the
    // lowest Threshold of them, where it is none (so that its runner lets it sleep only then, and a song played live plays as
    // its render); none otherwise. In Linear, at least its kernel's length, and its latency beside the dynamic bands' fall,
    // as their ears hear what came in that late. Worked out as its bands move, read on any thread.
    double getTailSeconds() const override { return tailSeconds.load (std::memory_order_relaxed); }

    // Its Phase is the one set by hand as it was prepared, as its latency is: Linear's (LinearPhase::getLatencyFor()), or
    // none. A Phase that automation moves plays as the one set by hand, as a Look-Ahead beyond its latency does in the
    // compressor; set by hand, its model makes it anew (model/playback/internal/SongPlayer.h).
    int getLatencySamples() const override { return linear ? linearPhase.getLatency() : 0; }
    int getLatencySamplesFor (std::span<const float> valuesSetByHand) const override;
    void takeValuesBeforePrepare (std::span<const float> valuesSetByHand) override;
    bool isLinear() const noexcept { return linear; }
    const LinearPhase& getLinearPhase() const noexcept { return linearPhase; } // for tests and benchmarks

private:
    static constexpr int stretch = 32; // samples between updates of moving coefficients

    using Filters = std::array<std::array<dsp::Filter, maxSections>, 2>; // a band's, per channel (or the middle or side)

    struct Band
    {
        bool on = false;
        int type = (int) BandType::bell, slope = 1, placement = (int) Placement::stereo;
        float frequency = 1000.0f, gain = 0.0f, q = 1.0f; // smoothed toward the parameters
        float dynamic = 0.0f, threshold = 0.0f;           // smoothed too
        float played = 0.0f;                              // the gain its sections were made with, its Dynamic's move included
        int numSections = 0;
        std::array<dsp::FilterCoefficients, maxSections> sections {};
        Filters filters {};

        // A change of placement while it plays: the band as it played, on the placement it had, fading out as the band on
        // its new placement fades in, for so many more samples.
        Filters fadingFilters {};
        int fadingPlacement = (int) Placement::stereo, fadeLeft = 0;

        // A dynamic band's ear: its region of what comes in (one filter for each side on Stereo), and the level there: the
        // loudest of each of the last milliseconds and of the one going on, the loudest of those before it, the level that
        // follows them, and the floor under which it is none.
        bool hearing = false; // its ear runs, set for the band as it is
        dsp::FilterCoefficients earShape {};
        std::array<dsp::Filter, 2> ear {};
        std::array<float, earHoldMilliseconds> peaks {};
        int peakIndex = 0, peakSamples = 0;
        float peakNow = 0.0f, peakBefore = 0.0f, level = 0.0f, floorLevel = 0.0f;
    };

    double sampleRate = 44100.0;
    float smoothing = 0.1f;               // of each step's distance to the target, per stretch of samples
    int fadeSamples = 441;                // a change of placement's crossfade
    float earRise = 0.0f, earFall = 0.0f; // how much of the way a dynamic band's level is left to go after a sample
    int samplesPerPeak = 44;              // a millisecond of them, for its ear's loudest
    std::int64_t songPlace = 0;           // where the song is, as its milliseconds are counted from it (its steps' way)
    std::atomic<double> tailSeconds { 0.0 };
    float outputGain = 1.0f, outputFrom = 1.0f; // as the stretch under way ends, and as it started
    dsp::ControlGrid grid { stretch };          // the song's grid of them (dsp/ControlGrid.h)
    std::array<Band, numBands> bands {};
    std::array<LiveValue, numBands> gainsMoved;
    SampleTap outputTap;

    // A stretch's samples: what came in, for the dynamic bands' ears (while any hears: anyHearing), and what one ear hears; the
    // middle and the side, for a band on either; and a band fading out, on its placement before a change.
    std::array<std::array<float, stretch>, 2> input {}, midSide {}, fading {};
    std::array<float, stretch> earInput {};
    bool anyHearing = false;

    // Linear phase: the values set by hand before it was prepared, whether it plays Linear, and its kernels. In Linear the
    // bands' sections play only what a dynamic band's Dynamic moves, after the kernels, and its ear hears what came in as
    // late as the kernels play it.
    std::vector<float> valuesBeforePrepare;
    bool linear = false;
    LinearPhase linearPhase;
    static_assert (LinearPhase::maxLength >= stretch);

    void update (Band&, int band, std::span<const float> parameters, bool jump);
    void hear (Band&, int numChannels, int length);
    void follow (Band&, float heard) const noexcept; // a sample's level, into the band's
    void play (Band&, int numChannels, float* const* channels, int length);
    void playOn (int where, Filters&, const Band&, int numChannels, float* const* channels, int length);
};
} // namespace phazite::devices
