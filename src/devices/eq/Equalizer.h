// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's EQ (roadmap step 1.6): eight bands, each a low cut, low shelf, bell, notch, high shelf or high cut, then an
// output gain. A new EQ changes nothing: its cuts are off and its other bands at 0 dB. Its parameters move smoothly, so
// turning a knob doesn't click, and it taps what it plays for its editor's analyzer.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"
#include "devices/core/SampleTap.h"
#include "devices/dsp/Filters.h"

#include <array>

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

    // A band's parameters, in this order: band b's are from b * fieldsPerBand on. The output gain is last.
    enum Field
    {
        on,
        type,
        frequency,
        gain,
        q,
        slope // a cut's steepness: 12, 24, 36 or 48 dB per octave
    };

    static constexpr int fieldsPerBand = 6;
    static constexpr int outputParameter = numBands * fieldsPerBand;
    static constexpr int getParameterIndex (int band, Field field) noexcept { return band * fieldsPerBand + (int) field; }

    static const DeviceInfo info;

    // The EQ's gain (not in decibels) at a frequency for the parameters' values given in the device's order and ranges,
    // as it plays them once settled: for its editor's curve and for tests. getBandMagnitude() gives one band's.
    static double getMagnitude (std::span<const float> parameters, double frequency, double sampleRate);
    static double getBandMagnitude (std::span<const float> parameters, int band, double frequency, double sampleRate);

    // What it played, for its editor's analyzer (devices/core/SampleTap.h).
    SampleTap& getOutputTap() noexcept { return outputTap; }

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;

private:
    static constexpr int maxSections = 4; // a 48 dB per octave cut

    struct Band
    {
        bool on = false;
        int type = (int) BandType::bell, slope = 1;
        float frequency = 1000.0f, gain = 0.0f, q = 1.0f; // smoothed toward the parameters
        int numSections = 0;
        std::array<dsp::FilterCoefficients, maxSections> sections {};
        std::array<std::array<dsp::Filter, maxSections>, 2> filters {}; // per channel
    };

    double sampleRate = 44100.0;
    float smoothing = 0.1f; // of each step's distance to the target, per stretch of samples
    float outputGain = 1.0f;
    std::array<Band, numBands> bands {};
    SampleTap outputTap;

    void update (Band&, int band, std::span<const float> parameters, bool jump);
};
} // namespace phazite::devices
