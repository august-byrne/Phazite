// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's compressor (roadmap step 1.6): it turns the sound down by the ratio as far as its level goes over the
// threshold, easing in across a soft knee, with an attack, a release and makeup gain. Its detector listens to the sound
// itself, or to a sidechain (another track, for ducking), through a low cut so that bass notes don't pump it, and turns
// both channels down alike, so that the stereo image holds. Its meters show how loud the detector hears the sound and
// how far the compressor turns it down.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/MeterValue.h"
#include "devices/core/Processor.h"
#include "devices/dsp/Envelopes.h"
#include "devices/dsp/Filters.h"

#include <array>

namespace phazite::devices
{
class Compressor final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteCompressor";

    enum Parameter
    {
        threshold,
        ratio,
        attack,
        release,
        knee,
        makeup,
        sidechainLowCut,
        mix // applied by its thin plug-in (DeviceInfo.h, mixParameterID)
    };

    static const DeviceInfo info;

    // How many decibels the compressor turns a steady level (in decibels) down by once it has settled, for the
    // parameters' values in the device's order: its curve, for its editor's graph and for tests.
    static float getReduction (std::span<const float> parameters, float levelDecibels);

    // The loudest the detector heard, in decibels, and the most the compressor turned the sound down by, in decibels
    // (0 or more), since its editor last looked (devices/core/MeterValue.h).
    MeterValue& getLevelMeter() noexcept { return levelMeter; }
    MeterValue& getReductionMeter() noexcept { return reductionMeter; }

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;

private:
    double sampleRate = 44100.0;
    float makeupSmoothing = 0.001f; // of the way to the makeup gain, per sample
    float makeupDecibels = 0.0f;
    float lastGainDecibels = 0.0f, lastGain = 1.0f; // the gain last worked out
    bool isFirstBlock = true;
    dsp::LevelFollower reduction;          // in decibels: rising with the attack, falling with the release
    std::array<dsp::Filter, 2> lowCuts {}; // the detector's, per channel
    MeterValue levelMeter { -120.0f }, reductionMeter { 0.0f };
};
} // namespace phazite::devices
