// SPDX-License-Identifier: AGPL-3.0-or-later
// The EQ's linear phase (roadmap step S6.4): its bands played as kernels symmetric about their middle tap, so that every
// frequency comes out as late as every other, through a partitioned convolution (dsp/PartitionedConvolution.h).
//
// Sizes, at a rate: r = 1 up to 50 kHz, 2 up to 100 kHz, 4 above; its latency L = 4096 r samples, partitions of P = 256 r,
// kernels of L - P taps to each side of their middle, so that a partition's wait and a kernel's half add up to L.
//
// Each band that is on is, at each frequency, a real gain on what it is placed on (its magnitude as the EQ's minimum phase
// plays it: Equalizer::BandShape), and the bands in their order a 2 x 2 real matrix from (left, right) in to (left, right)
// out, whose four entries are four kernels' responses: one kernel for both sides while every band is on Stereo; while none
// is on Left or Right, two: the middle's and the side's ((left + right) / 2 and (left - right) / 2, taken apart from the
// sides' spectra) where one of them is a delay alone (the bands on Mid alone, or on Side alone), so that a Side band leaves
// a sound alike on both sides as it was, to the bit, and else the sides' and the crossings' (each crossing designed whole,
// not the difference of two large kernels, so that its rounding is its own); else four; on a mono channel, one, of the bands
// that play on it. A kernel that is a delay alone plays what came in as late as the latency, to the bit. A kernel's design:
// its response on a grid of 2 L frequencies, turned into time by an inverse FFT, cut to its taps under a window, moved so
// that its sum is its response at 0 Hz, cut into partitions and each partition transformed.
//
// Its kernels change on a grid of L / 2 samples counted from the song's place (dsp/ControlGrid.h): at a line, the values
// (not the 32-sample glide's) are taken, and where they differ from the kernels designed last a design starts, spread evenly
// over the next L / 2 samples played by a count of work, the same every time; at the first line after it is done the
// kernels played glide to it over L / 2 samples, both convolved meanwhile and their outputs crossfaded in a straight line.
// So the values taken at a line are wholly in the sound L samples later, as the sound that came in with them comes out.
// A change while a glide or a design is under way waits for it. After a reset the history is silence; where the values
// differ from the kernels then, a design starts at once and is put in whole, without a glide, L / 2 samples later. Where the
// song jumps, a design under way for other values is dropped, and one for the values there starts at once, spread over the
// samples to its grid's next line so that it glides in there (where L / 8 samples or more are left; else the next line's
// design follows as at any line); a glide under way ends as it would.
// Real-time safe once prepared.
#pragma once

#include "devices/dsp/ControlGrid.h"
#include "devices/dsp/Filters.h"
#include "devices/dsp/PartitionedConvolution.h"
#include "devices/dsp/SplitFft.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace phazite::devices
{
class LinearPhase
{
public:
    static constexpr int latencyAtBaseRate = 4096, partitionAtBaseRate = 256;
    static constexpr int maxLength = 32; // samples a call to process() takes at most: one of the EQ's steps

    // r: 1 for rates up to 50 kHz, 2 up to 100 kHz, 4 above.
    static int getRateFactor (double sampleRate) noexcept;
    static int getLatencyFor (double sampleRate) noexcept { return latencyAtBaseRate * getRateFactor (sampleRate); }

    // How its kernels are designed: the grid of frequencies, as so many times the latency; the window's Kaiser beta (0, none);
    // and the part of each side's end tapered by a raised cosine (0, none). After the cut, every tap is moved by the same small
    // amount, so that the kernel's sum is its response at 0 Hz. Measured (tests/EqualizerTests.cpp, "the window and the
    // grid"): a Kaiser smears the curve, the more the longer a band rings (beta 8 put a bell at 200 Hz, Q 4, 0.6 dB off; the
    // rectangle 0.005 dB), and cuts off less of the low end; a grid of 4 L halves the hardest low-end errors at twice the
    // design's work; a raised cosine over the last 2, 3 or 5 % of each end left the curve from 200 Hz up as it was but put a
    // 48 dB cut's level at 10 Hz 3 to 6 dB higher at 30 Hz and 9 to 13 dB at 80 Hz. So a rectangle, 2 L.
    struct Design
    {
        int gridTimesLatency = 2;
        double windowBeta = 0.0;
        double taperFraction = 0.0;
    };

    // At a rate, its kernels designed whole for the EQ's values (in the order and ranges of Equalizer::info) on so many
    // channels, by a design (by default, Design's). Allocates.
    void prepare (double sampleRate, std::span<const float> values, int numChannels = 2);
    void prepare (double sampleRate, std::span<const float> values, int numChannels, Design);
    bool isPrepared() const noexcept { return latency > 0; }

    // Silence in its history; its kernels as they were (a glide under way is put in whole, a design under way given up).
    void reset() noexcept;

    int getLatency() const noexcept { return latency; }
    int getPartitionSize() const noexcept { return convolution.getPartitionSize(); }
    int getNumPartitions() const noexcept { return convolution.getNumPartitions(); }

    // How long its sound goes on after what comes in stops: its kernel's length, and the partition's wait.
    double getTailSeconds() const noexcept { return sampleRate > 0.0 ? 2.0 * latency / sampleRate : 0.0; }

    // As a block starts: whether the song plays and where it is (its grid's place, and whether the song jumped: a loop going
    // round, a start elsewhere).
    void startBlock (bool isPlaying, std::int64_t songSample) noexcept;

    // Up to maxLength samples, in place, as late as its latency: through its kernels, gliding where they glide. It walks its own
    // grid of L / 2 samples through them, wherever its lines fall among the EQ's steps: at each line, with the values then
    // (the block's) and the channels played, its kernels change if they must.
    void process (float* const* channels, int numChannels, int length, std::span<const float> values) noexcept;

    // What came in as late as its latency, for the samples process() played last (each side's: a dynamic band's ear).
    const float* getDelayed (int channel) const noexcept { return delayed[(size_t) channel].data(); }

    // For tests and benchmarks: the work of the design started last, in units of about five floating-point operations,
    // and all the work done since it was prepared; the designs started; whether it glides; and the multiply-adds a sample
    // the kernels it plays take.
    std::int64_t getDesignCost() const noexcept { return designCost; }
    std::int64_t getWorkDone() const noexcept { return workDone; }
    int getDesignsStarted() const noexcept { return designsStarted; }
    bool isGliding() const noexcept { return gliding >= 0; }
    int getNumKernelsPlayed() const noexcept;
    int getMultiplyAddsPerSample (int numChannels) const noexcept;

    // The taps of the first kernel of the set played (left to left), for tests.
    std::vector<float> getPlayedTaps (int kernel = 0) const;

    // The number of values compared to know whether its kernels must change: each band's first seven (on, type,
    // frequency, gain, Q, slope, placement), and whether it plays on one channel.
    static constexpr int numBands = 8, fieldsCompared = 7, numCompared = numBands * fieldsCompared + 1;

private:
    using Complex = dsp::PartitionedConvolution::Complex;
    using Kernels = dsp::PartitionedConvolution::Kernels;
    using Compared = std::array<float, numCompared>;

    enum class StepKind
    {
        response,
        fill,
        transform,
        window,
        zeroHertz,
        partitionFill,
        partitionTransform,
        partitionTakeApart
    };

    struct Step
    {
        StepKind kind = StepKind::response;
        int a = 0, b = 0;
        int cost = 0;
    };

    struct Band
    {
        bool played = false;
        int placement = 0;
        int numSections = 0;
        std::array<dsp::FilterCoefficients, 4> sections {};
    };

    double sampleRate = 0.0;
    int latency = 0, half = 0, taps = 0, gridPoints = 0, bins = 0;
    dsp::PartitionedConvolution convolution;
    dsp::ControlGrid grid { 1 };

    // Three sets of kernels: played, glided to, and in design.
    std::array<Kernels, 3> sets;
    std::array<Compared, 3> setValues {};
    int played = 0, gliding = -1, glideDone = 0;
    bool afterReset = true;

    // Where the song is due to be at the next sample, while it plays and that is known; and whether it jumped.
    std::int64_t nextSongSample = 0;
    bool placeKnown = false, jumped = false;

    // The design under way.
    bool designing = false, putInWhole = false;
    bool droppedLast = false; // the design before this one was dropped at a jump: this one is kept, so that one in two ends
    int designSet = 0, designSpread = 1, numKernels = 1, numResponses = 1, numSteps = 0, nextStep = 0;
    bool mayPlayMidSide = false; // its middle and side, if one of them turns out to be a delay alone
    std::int64_t designPlayed = 0, designCost = 0, designDone = 0, workDone = 0;
    int designsStarted = 0;
    std::array<Band, numBands> bands {};
    std::array<int, 4> entries {}; // each response's entry of the matrix: 0 left to left, 1 right to left, 2 left to right,
    std::array<int, 4> routes {};  // 3 right to right, 4 the mono channel's, 5 the middle's, 6 the side's; and the routes
                                   // from each side to each
    std::array<double, 4> largest {}, offOne {};
    std::array<double, 2> tapSums {};   // the sums of the two kernels the window step works on, and what each tap moves by
    std::array<float, 2> tapsMoveBy {}; // so that each kernel's sum is its response at 0 Hz
    std::vector<Step> steps;
    std::vector<double> tangents;                 // tan (pi f / rate) at each frequency of the grid, as dsp::getResponse() has it
    std::vector<float> window;                    // from the middle tap out
    std::array<std::vector<float>, 4> responses;  // each kernel's, on the grid's frequencies from 0 to half the rate
    std::array<std::vector<float>, 4> kernelTaps; // each kernel's taps
    std::vector<Complex> bigIn, bigWork, bigOut;  // the inverse transform of two kernels' responses at once
    std::vector<Complex> smallIn, smallWork, smallOut, spare; // two partitions' transform at once
    dsp::SplitFft bigFft, smallFft;

    // What came in, for its delayed copy, and the stretch's.
    std::array<std::vector<float>, 2> history;
    int historyMask = 0, historyAt = 0;
    std::array<std::array<float, maxLength>, 2> delayed {}, glided {};

    Compared compare (std::span<const float> values, int numChannels) const noexcept;
    void startDesign (const Compared&, std::span<const float> values, int set, bool whole, int spread) noexcept;
    void atJump (const Compared&, std::span<const float> values) noexcept;
    void atLine (const Compared&, std::span<const float> values) noexcept;
    bool glideInIfDone() noexcept;      // a design done glides in from here, once a glide under way has ended
    bool doStep (const Step&) noexcept; // whether it did work: none for a kernel that turned out to be none or a delay
    void advanceDesign (int samples) noexcept;
    void finishResponse() noexcept;
    int getFreeSet() const noexcept;
    void playStretch (float* const* channels, int numChannels, int from, int length) noexcept;
    void play (float* const* channels, int numChannels, int from, int length) noexcept;
    void finish (const Kernels&, float* const* out, int numChannels, int from, int length) const noexcept;
};
} // namespace phazite::devices
