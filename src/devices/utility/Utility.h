// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's utility (roadmap step 2.10): the small fixes a mix needs, in one device: a gain, a balance between the left
// and the right, the stereo width (from mono to twice as wide, through mid and side), the bass in mono below a frequency
// (so it stays solid on a club's speakers), and each side's polarity flipped. It changes nothing until turned, and has no
// mix: what it puts out is the sound itself.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"
#include "devices/dsp/Crossover.h"

#include <array>

namespace phazite::devices
{
class Utility final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteUtility";

    enum Parameter
    {
        gain,
        balance,
        width,
        bassMono,
        bassMonoFrequency,
        invertLeft,
        invertRight
    };

    static const DeviceInfo info;

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;

private:
    double sampleRate = 44100.0;
    float smoothing = 0.001f; // of the way to new gains, per sample
    float leftGain = 1.0f, rightGain = 1.0f, sideGain = 1.0f;
    bool isFirstBlock = true;
    std::array<dsp::Crossover, 2> crossovers; // the bass mono's split, per channel
    float shownFrequency = 0.0f;              // the crossovers' frequency
};
} // namespace phazite::devices
