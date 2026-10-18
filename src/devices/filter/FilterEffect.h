// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's filter (roadmap step 2.10), the "Filter" device: a resonant low-pass, high-pass, band-pass or notch filter
// to sweep a sound open or closed, 12 or 24 dB per octave, with a drive that saturates what goes into it; or a comb
// (step 2.14: Comb + and Comb -, dsp/CombFilter.h), which has no slope, whose cutoff sets the pitch of its peaks and whose
// resonance makes them ring. Its cutoff glides rather than jumps, so automation, modulators and a turned knob sweep it
// smoothly. A new filter changes little: a low-pass open all the way.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"
#include "devices/dsp/CombFilter.h"
#include "devices/dsp/Filters.h"

#include <array>
#include <atomic>

namespace phazite::devices
{
class FilterEffect final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteFilter";

    enum Parameter
    {
        type,
        cutoff,
        resonance,
        slope,
        drive,
        mix // applied by its thin plug-in (DeviceInfo.h, mixParameterID)
    };

    // Saved in projects by number: new types go at the end.
    enum class Type
    {
        lowPass,
        highPass,
        bandPass,
        notch,
        combPlus, // step 2.14
        combMinus
    };

    static const DeviceInfo info;

    // Its response at a frequency for the parameters' values (a gain, not in decibels): for its editor and tests.
    static double getMagnitude (std::span<const float> parameters, double frequency, double sampleRate);

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    double getTailSeconds() const override; // a comb's ring

private:
    static constexpr int coefficientStep = 8; // samples between new coefficients while the cutoff glides

    double sampleRate = 44100.0;
    float glide = 0.001f;   // of the way to a new cutoff, per sample (in octaves)
    float octaves = 10.0f;  // the cutoff now, in octaves above 20 Hz
    float driveGain = 1.0f; // smoothed
    float driveSmoothing = 0.001f;
    bool isFirstBlock = true;
    std::array<std::array<dsp::Filter, 2>, 2> stages {}; // [section][channel]
    std::array<dsp::CombFilter, 2> combs;                // [channel]
    bool wasComb = false;                                // whether the last block played a comb
    std::atomic<float> tailSeconds { 0.0f };
};
} // namespace phazite::devices
