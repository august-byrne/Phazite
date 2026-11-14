// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's saturator (roadmap step 2.10): drives the sound into a curve that rounds its peaks off, adding harmonics
// that make it warmer, fuller or grittier: soft (like a transistor), tape (gentler and wider), tube (lopsided, so it
// adds even harmonics too), hard (clipping) or fold (peaks folded back, for a synthetic edge). Auto gain takes the drive
// back off its level, so turning it up changes the colour rather than the loudness; a tone tilts it darker or brighter
// afterwards. It runs at twice the sample rate and plays each curve by its antiderivative (dsp/Saturation.h), so the
// harmonics it adds don't fold back down as aliasing, however hard it is driven.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"
#include "devices/dsp/Filters.h"
#include "devices/dsp/Saturation.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <memory>

namespace phazite::devices
{
class Saturator final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteSaturator";

    enum Parameter
    {
        drive,
        shape,
        tone,
        output,
        autoGain,
        mix // applied by its runner (DeviceInfo.h, mixParameterID)
    };

    enum class Shape
    {
        soft,
        tape,
        tube,
        hard,
        fold
    };

    static const DeviceInfo info;

    // The curve itself, for a sample already driven: for tests and for drawing it.
    static float shapeSample (Shape, float driven) noexcept;

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    int getLatencySamples() const override { return latency; }
    bool delaysDry() const noexcept override { return true; }

private:
    double sampleRate = 44100.0;
    int latency = 0;
    float smoothing = 0.001f; // of the way to new gains, per sample at twice the rate
    float driveGain = 1.0f, outputGain = 1.0f;
    bool isFirstBlock = true;
    Shape lastShape = Shape::soft;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    std::array<dsp::AntialiasedCurve, 2> curves;

    // The copy of what came in for its mix (ProcessContext::dry), through filters like the sound's and a straight line in
    // the curve's place, while the mix uses it: the half-band filters turn the phase of the top octave, and the copy is
    // turned and delayed as the sound is, so that the two meet in phase at any mix.
    std::unique_ptr<juce::dsp::Oversampling<float>> dryOversampling;
    std::array<dsp::AntialiasedCurve, 2> dryCurves;
    float lastMix = 1.0f;   // the mix in its last block
    bool dryInStep = false; // the copy went through its filters in the last block
    std::array<dsp::Filter, 2> dcBlockers {}, lowTilts {}, highTilts {};
};
} // namespace phazite::devices
