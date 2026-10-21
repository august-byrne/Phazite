// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's maximizer (roadmap step 2.10), in the style of FL Studio's Maximus: it makes a mix loud. The sound, raised by
// its gain, splits into low, mid and high bands, each squeezed by a compressor of its own and set to a level, so that a
// loud kick doesn't pull the whole mix down; the bands then go through Phazite's limiter, which keeps every sample, and
// the peaks between them, under the ceiling. With no gain and no squeeze, it is the limiter alone.
#pragma once

#include "Limiter.h"
#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"
#include "devices/dsp/Crossover.h"
#include "devices/dsp/Envelopes.h"

#include <array>

namespace phazite::devices
{
class Maximizer final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteMaximizer";
    static constexpr int numBands = 3;

    enum Parameter
    {
        gain,
        lowFrequency,
        highFrequency,
        lowSqueeze,
        midSqueeze,
        highSqueeze,
        lowLevel,
        midLevel,
        highLevel,
        ceiling,
        release,
        mix // applied by its thin plug-in (DeviceInfo.h, mixParameterID)
    };

    static const DeviceInfo info;

    // How many decibels a band at a steady level (in decibels, after the gain) is turned down once settled, for a
    // squeeze from 0 to 1: its curve, for tests and its editor.
    static float getReduction (float squeeze, float levelDecibels);

    // The limiter's meters (Limiter.h): what comes in, how far it turns the sound down, and what goes out.
    Limiter& getLimiter() noexcept { return limiter; }

    // Where each band's squeeze begins, in decibels (for its editor).
    static constexpr float squeezeThreshold = -12.0f;

    // Each band as its editor shows it, since it last looked (devices/core/MeterValue.h): its level coming in, in decibels
    // (after the gain), and the most it was squeezed, in decibels.
    MeterValue& getBandLevel (int band) noexcept { return bandLevels[(size_t) band]; }
    MeterValue& getBandReduction (int band) noexcept { return bandReductions[(size_t) band]; }

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    int getLatencySamples() const override { return limiter.getLatencySamples(); }

private:
    double sampleRate = 44100.0;
    float smoothing = 0.001f; // of the way to new gains, per sample
    float inGain = 1.0f;
    std::array<float, numBands> levels { 1.0f, 1.0f, 1.0f };
    bool isFirstBlock = true;
    float shownLow = 0.0f, shownHigh = 0.0f; // the crossovers' frequencies
    std::array<dsp::ThreeBandSplit, 2> splits;
    std::array<dsp::LevelFollower, numBands> reductions; // in decibels
    Limiter limiter;
    std::array<float, Limiter::mix + 1> limiterValues {}; // its parameters, as the maximizer sets them
    std::array<MeterValue, numBands> bandLevels { MeterValue { -120.0f }, MeterValue { -120.0f }, MeterValue { -120.0f } };
    std::array<MeterValue, numBands> bandReductions; // from 0
};
} // namespace phazite::devices
