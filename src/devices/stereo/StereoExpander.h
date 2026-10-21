// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's stereo expander (roadmap step 2.17, in place of the Utility): makes a sound wider or narrower without changing
// how it sounds in mono. Width turns the side (what differs between the left and the right) down to mono or up to twice as
// wide. Spread makes stereo from the middle, for a mono sound: it adds to the side a copy of the middle smeared over 20 ms
// by velvet noise (Alary et al., 2017), which sounds wide, takes no delay and, since the left plus the right stays the
// middle twice, never changes the mono sum, unlike a Haas delay's comb. Bass Mono keeps what is below a frequency in the
// middle, so it stays solid on a club's speakers; the bass is never spread. Each side's polarity can flip, and a gain
// follows. It changes nothing until turned, and has no mix. Its editor shows how alike the two sides are, coming in and
// going out.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/LiveValue.h"
#include "devices/core/Processor.h"
#include "devices/dsp/Crossover.h"
#include "devices/dsp/DelayLine.h"
#include "devices/dsp/Filters.h"

#include <array>

namespace phazite::devices
{
class StereoExpander final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteStereoExpander";

    enum Parameter
    {
        width,
        spread,
        bassMono,
        bassMonoFrequency,
        invertLeft,
        invertRight,
        gain
    };

    static const DeviceInfo info;

    // How alike the two sides are, from -1 (each the other upside down) through 0 (unrelated) to 1 (the same: mono),
    // averaged over about half a second, coming in and going out, once a block (devices/core/LiveValue.h); noReading while
    // the sound is quieter than -60 dB.
    static constexpr float noReading = 2.0f;
    const LiveValue& getInputCorrelation() const noexcept { return inputCorrelation; }
    const LiveValue& getOutputCorrelation() const noexcept { return outputCorrelation; }

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    double getTailSeconds() const override { return decorrelatorSeconds; }

    static constexpr double decorrelatorSeconds = 0.02; // how long the spread smears the middle over
    static constexpr int numTaps = 30;                  // the velvet noise's pulses in that time

private:
    // How alike two signals are, from their running averages.
    struct Correlation
    {
        double product = 0.0, left = 0.0, right = 0.0;

        void add (float l, float r, double weight) noexcept
        {
            product += weight * ((double) l * r - product);
            left += weight * ((double) l * l - left);
            right += weight * ((double) r * r - right);
        }

        float get() const noexcept;
    };

    double sampleRate = 44100.0;
    float smoothing = 0.001f;     // of the way to new gains, per sample
    double averageWeight = 0.001; // of a new sample in the correlation's averages
    float leftGain = 1.0f, rightGain = 1.0f, levelGain = 1.0f, sideGain = 1.0f, spreadGain = 0.0f, bassAmount = 0.0f;
    bool isFirstBlock = true;
    float shownFrequency = 0.0f; // the bass mono's and the spread's high-pass frequency

    dsp::Crossover sideSplit, middlePhase;        // the side's low cut, and the middle kept in phase with it
    std::array<dsp::Filter, 2> spreadHighPass {}; // the middle the spread makes stereo from, without its bass (24 dB per octave)
    dsp::FilterCoefficients highPass {};
    dsp::DelayLine middleLine;             // what the velvet noise reads
    std::array<int, numTaps> tapDelays {}; // in samples
    std::array<float, numTaps> tapGains {};
    Correlation inputAverage, outputAverage;
    LiveValue inputCorrelation { noReading }, outputCorrelation { noReading };

    void setFrequency (float hertz) noexcept;
};
} // namespace phazite::devices
