// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's limiter (roadmap step 1.6), the master's last device: it raises the sound by its gain and keeps it under the
// ceiling. It looks ahead, so it turns the sound down smoothly before a peak arrives instead of clipping it, and it
// listens for the peaks between samples too ("true peaks", which a converter or a lossy encoder can bring out), as
// streaming services' loudness rules ask. Looking ahead delays the sound by about 2 ms, which the engine makes up for
// on the other tracks. No sample ever goes over the ceiling.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/MeterValue.h"
#include "devices/core/Processor.h"
#include "devices/dsp/DelayLine.h"

#include <array>
#include <cstdint>
#include <vector>

namespace phazite::devices
{
class Limiter final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteLimiter";
    static constexpr double lookaheadSeconds = 0.002;

    enum Parameter
    {
        gain,
        ceiling,
        release,
        mix // applied by its thin plug-in (DeviceInfo.h, mixParameterID)
    };

    static const DeviceInfo info;

    // Since its editor last looked (devices/core/MeterValue.h), in decibels: the loudest sample coming in, after the
    // gain; the most it turned the sound down by (0 or more); and the loudest sample going out.
    MeterValue& getInputMeter() noexcept { return inputMeter; }
    MeterValue& getReductionMeter() noexcept { return reductionMeter; }
    MeterValue& getOutputMeter() noexcept { return outputMeter; }

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    int getLatencySamples() const override { return latency; }
    const FallingValue* getGainReduction() const noexcept override { return &gainReduction; }

private:
    // Peaks between samples are found at a quarter, a half and three quarters of the way from one to the next, each
    // interpolated from the eight samples around it.
    static constexpr int taps = 8, points = 3;

    double sampleRate = 44100.0;
    int lookahead = 1, latency = 0;
    float gainSmoothing = 0.001f; // of the way to the gain, per sample
    float appliedGain = 0.0f;
    bool isFirstBlock = true;

    std::array<std::array<float, taps>, points> interpolator {};
    std::array<std::array<float, taps>, 2> recent {}; // each channel's last samples, newest last
    float lastBetween = 0.0f;                         // the loudest point between the two samples before

    // The gain each sample needs, held at the least of the last lookahead samples (a running minimum, kept in order),
    // then let back up at the release time, then averaged over the lookahead so that it moves smoothly.
    std::vector<float> heldValues;
    std::vector<std::int64_t> heldTimes;
    int heldFirst = 0, heldCount = 0;
    std::int64_t now = 0;
    float released = 1.0f;
    std::vector<float> averaged;
    int averagedNext = 0;
    double averagedSum = 0.0;

    std::array<dsp::DelayLine, 2> delays;
    MeterValue inputMeter { -120.0f }, reductionMeter { 0.0f }, outputMeter { -120.0f };
    FallingValue gainReduction { reductionFallPerSecond };

    float hold (float required) noexcept;
    float average (float value) noexcept;
};
} // namespace phazite::devices
