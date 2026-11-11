// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's reverb (roadmap step 1.6): a space around the sound, of one of two kinds, as its Mode says.
//
// A Room, small or a hall as its Size says: after a pre-delay and a few allpass filters that smear the attack, eight delay
// lines feed each other through a mixing matrix that keeps the sound's energy (a "feedback delay network"); each line
// loses the treble faster than the bass, past the damping frequency, as much on a pass as its length asks (so that the
// tail keeps its colour as it fades), and fades by 60 dB over the decay time. The lines' lengths move a little, each at
// its own slow rate, so the tail doesn't ring like metal; their starting points come from a fixed seed, so it plays the
// same every time.
//
// A Plate, the studios' sheet of steel as Jon Dattorro's paper builds it ("Effect Design, Part 1: Reverberator and Other
// Filters", J. Audio Eng. Soc. 45 (9), 1997, its figure 1 and table 2), written from the paper: both sides of the sound
// as one, four allpass filters that smear it, then a tank of two halves that feed each other in a figure of eight, each an
// allpass filter whose length sways slowly, a delay, the damping and the decay's gain, another allpass filter and a delay;
// what comes out is seven taps of the tank for each side. It sounds 9 ms after the hit, where the room waits for its
// lines, and its tail lengthens a little as it fades. Its lengths are the paper's at the Size's middle, scaled to the
// sample rate; its Decay is measured, not worked out (the table in Reverb.cpp), since its allpass filters hold the sound
// longer than its loop alone would say. Its sway is fixed, as the paper's is: enough to break up the tank's ringing.
//
// In both, the Diffusion is how far the allpass filters smear the sound (at its middle, as the paper has them), the Damping
// is the frequency past which the treble fades sooner (a low-pass on each pass: the plate's, which its sound passes far less
// often, two octaves lower, so that where the treble starts to fade it fades as fast in either mode), a low cut keeps
// rumble out of the tail, a high cut takes its treble, by 12 dB an octave or by a gentle 6 (the plate's own "bandwidth", a
// low-pass on what goes in, is that: a filter before a reverb or after it comes to the same), and the width spreads it from
// mono to wide stereo. On an insert it starts at 25 % mix; on a return, all effect.
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
        mode,
        size,
        decay,
        predelay,
        diffusion,
        damping,
        lowCut,
        highCut,
        highCutSlope,
        width,
        mix // applied by its runner (DeviceInfo.h, mixParameterID)
    };

    // The Mode's choices, in their order.
    enum Mode
    {
        room,
        plate
    };

    static const DeviceInfo info;

    // How long the tail takes to fade by 60 dB at a frequency, in seconds, for the parameters' values in the device's
    // order: the decay for the bass, less for the treble past the damping frequency. For its editor's graph.
    static double getDecayAt (std::span<const float> parameters, double frequency, double sampleRate);

    // The plate as its parameters make it: how large it is, of the paper's (its Size, less where the Decay is too short for a
    // plate so large to fade in), how far its allpass filters smear (its Diffusion, less where the Decay is shorter than
    // they would ring), and its tank's gain, which makes the Decay. For tests.
    struct PlateSetting
    {
        double scale = 1.0, diffusion = 0.5, gain = 0.5;
    };

    static PlateSetting getPlateSetting (std::span<const float> parameters);

    // The Size at which a plate is as large as a scale says, for an editor that shows what a short decay holds it to.
    static double getPlateSize (double scale);

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    double getTailSeconds() const override;

private:
    static constexpr int numDiffusers = 4;
    static constexpr int numTankParts = 4; // in each half: the swaying allpass filter, a delay, an allpass filter, a delay
    static constexpr int numTaps = 7;      // of the tank, for each side

    // The plate's lengths, each in samples: its four diffusers', its tank's parts', and how far back each tap reads.
    static constexpr int numPlateLengths = numDiffusers + 2 * numTankParts + 2 * numTaps;

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

    // What the plate's sound goes through with values that glide: the tank's gain, the damping's coefficient, and the
    // allpass filters' gains (the input's two pairs, the tank's two).
    struct PlateValues
    {
        float gain = 0.0f, damping = 1.0f, input1 = 0.0f, input2 = 0.0f, tank1 = 0.0f, tank2 = 0.0f;
    };

    double sampleRate = 44100.0;
    std::array<dsp::DelayLine, 2> predelays;
    std::array<dsp::Filter, 2> lowCuts {}, highCuts {};
    std::array<float, 2> gentleCuts {}; // the high cut at 6 dB an octave: each side's low-pass
    float gentleShare = -1.0f;          // how much of the high cut is the 6 dB one's, gliding as its slope changes
    float gentleStep = 0.001f;          // per sample

    // The room.
    std::array<std::array<Allpass, numDiffusers>, 2> diffusers;
    std::array<dsp::DelayLine, numLines> lines;
    std::array<float, numLines> damped {}, phases {}, phaseSteps {};

    // The plate.
    std::array<dsp::DelayLine, numDiffusers> plateDiffusers;
    std::array<std::array<dsp::DelayLine, numTankParts>, 2> tank;
    std::array<float, 2> tankDamped {};
    std::array<float, numPlateLengths> plateLengths {}; // as they are now, gliding to where the Size puts them,
    std::array<float, numPlateLengths> plateAims {};    // after these, which glide there first
    PlateValues plateValues;
    float platePhase = 0.0f, platePhaseStep = 0.0f, plateSway = 0.0f;
    bool plateStarted = false;

    // What the plate's setting was last worked out from, and what it came to.
    std::array<float, 3> plateSettingFor { -1.0f, -1.0f, -1.0f };
    PlateSetting plateSetting;

    // Smoothed toward the parameters, so that turning a knob glides.
    float shownSize = -1.0f, shownPredelay = 0.0f, shownDiffusion = 0.5f;
    float smoothing = 0.001f; // of the way, per sample

    // The mode that plays, and how much of it: another fades in once this one has faded out.
    int playing = -1;
    float modeLevel = 1.0f, modeStep = 0.001f;
    float feedFrom = 1.0f, feedTo = 1.0f; // how much of what comes in the mode takes, over the block: all, but as it comes in

    std::atomic<float> tailSeconds { 2.0f };

    // The way out of either mode: the low cut, the high cut at its slope, and the width.
    struct Output
    {
        dsp::FilterCoefficients lowCut, highCut;
        float gentle = 1.0f, spread = 1.0f;
        bool cutsHigh = false, isGentle = false;
    };

    Output makeOutput (std::span<const float> parameters) const;
    void putOut (float& left, float& right, const Output&, float gain) noexcept;

    void processRoom (const ProcessContext&, int numChannels);
    void processPlate (const ProcessContext&, int numChannels);
    template <bool glides>
    void playPlate (const ProcessContext&, int numChannels, const PlateValues& from, const PlateValues& to,
                    const std::array<float, numPlateLengths>& lengthSteps);
    void resetRoom();
    void resetPlate();
};
} // namespace phazite::devices
