// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's multiband dynamics (roadmap step 2.10), in the style of OTT: the sound split into low, mid and high bands,
// each squeezed both ways at once, its loud parts turned down (downward compression) and its quiet parts brought up
// (upward compression), so details and tails come forward and everything sounds bigger and denser. Depth sets how much of
// that happens, time how fast it reacts; upward and downward set how much of each, and each band has a gain. The bands
// split with Linkwitz-Riley crossovers, so with depth at 0 the sound comes out as it went in.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"
#include "devices/dsp/Crossover.h"
#include "devices/dsp/Envelopes.h"

#include <array>

namespace phazite::devices
{
class Multiband final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteMultiband";
    static constexpr int numBands = 3;

    enum Parameter
    {
        depth,
        time,
        upward,
        downward,
        lowFrequency,
        highFrequency,
        lowGain,
        midGain,
        highGain,
        inputGain,
        outputGain,
        mix // applied by its thin plug-in (DeviceInfo.h, mixParameterID)
    };

    static const DeviceInfo info;

    // How many decibels a band at a steady level (in decibels, after the input gain) is turned up (above 0) or down,
    // once settled, for the parameters' values: its curve, for tests and its editor.
    static float getGainChange (std::span<const float> parameters, float levelDecibels);

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;

private:
    double sampleRate = 44100.0;
    float smoothing = 0.001f; // of the way to new gains, per sample
    float inGain = 1.0f, outGain = 1.0f;
    std::array<float, numBands> bandGains { 1.0f, 1.0f, 1.0f };
    bool isFirstBlock = true;
    float shownLow = 0.0f, shownHigh = 0.0f; // the crossovers' frequencies
    std::array<dsp::ThreeBandSplit, 2> splits;
    std::array<dsp::LevelFollower, numBands> changes; // each band's gain change, in decibels below 0 dB of reduction
};
} // namespace phazite::devices
