// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's reverb (roadmap step 1.6): a room or hall around the sound. After a pre-delay and a few allpass filters that
// smear the attack, eight delay lines feed each other through a mixing matrix that keeps the sound's energy (a
// "feedback delay network"); each line loses the treble faster than the bass, past the damping frequency, and fades
// by 60 dB over the decay time. The lines' lengths move a little, each at its own slow rate, so the tail doesn't ring
// like metal; their starting points come from a fixed seed, so it plays the same every time. A low cut keeps rumble out
// of the tail, and the width spreads it from mono to wide stereo. On an insert it starts at 25 % mix; on a return,
// all effect.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"
#include "devices/dsp/DelayLine.h"
#include "devices/dsp/Filters.h"

#include <array>
#include <atomic>

namespace phazite::devices
{
class Reverb final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteReverb";
    static constexpr int numLines = 8;

    enum Parameter
    {
        size,
        decay,
        predelay,
        damping,
        lowCut,
        width,
        mix // applied by its thin plug-in (DeviceInfo.h, mixParameterID)
    };

    static const DeviceInfo info;

    // How long the tail takes to fade by 60 dB at a frequency, in seconds, for the parameters' values in the device's
    // order: the decay for the bass, less for the treble past the damping frequency. For its editor's graph.
    static double getDecayAt (std::span<const float> parameters, double frequency, double sampleRate);

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    double getTailSeconds() const override;

private:
    static constexpr int numDiffusers = 4;

    struct Allpass
    {
        dsp::DelayLine line;
        int length = 1;
        float gain = 0.7f;

        float process (float input) noexcept
        {
            const auto delayed = line.read (length - 1);
            const auto output = delayed - gain * input;
            line.push (input + gain * output);
            return output;
        }
    };

    double sampleRate = 44100.0;
    std::array<dsp::DelayLine, 2> predelays;
    std::array<std::array<Allpass, numDiffusers>, 2> diffusers;
    std::array<dsp::DelayLine, numLines> lines;
    std::array<float, numLines> damped {}, phases {}, phaseSteps {};
    std::array<dsp::Filter, 2> lowCuts {};

    // Smoothed toward the parameters, so that turning a knob glides.
    float shownSize = -1.0f, shownPredelay = 0.0f;
    float smoothing = 0.001f; // of the way, per sample
    std::atomic<float> tailSeconds { 2.0f };
};
} // namespace phazite::devices
