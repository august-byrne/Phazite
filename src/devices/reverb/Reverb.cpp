// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Reverb.h"
#include "devices/dsp/Oscillators.h"
#include "devices/dsp/Random.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
namespace
{
    constexpr std::string_view modes[] = { "Room", "Plate" };
    constexpr std::string_view slopes[] = { "6 dB", "12 dB" }; // an octave

    // The IDs are saved in projects.
    constexpr ParameterInfo parameters[] = {
        { .id = "mode", .name = "Mode", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::choice, .choices = modes },
        { .id = "size", .name = "Size", .defaultValue = 0.5f, .unit = Unit::percent },
        { .id = "decay",
          .name = "Decay",
          .minimum = 0.2f,
          .maximum = 20.0f,
          .defaultValue = 2.0f,
          .unit = Unit::seconds,
          .midpoint = 2.0f },
        { .id = "predelay",
          .name = "Pre-Delay",
          .minimum = 0.0f,
          .maximum = 250.0f,
          .defaultValue = 10.0f,
          .unit = Unit::milliseconds,
          .midpoint = 40.0f },
        { .id = "diffusion", .name = "Diffusion", .defaultValue = 0.5f, .unit = Unit::percent },
        { .id = "damping",
          .name = "Damping",
          .minimum = 1000.0f,
          .maximum = 20000.0f,
          .defaultValue = 6000.0f,
          .unit = Unit::hertz,
          .midpoint = 4472.0f }, // the middle of the range, on a log scale
        { .id = "lowCut",
          .name = "Low Cut",
          .minimum = 20.0f,
          .maximum = 1000.0f,
          .defaultValue = 100.0f,
          .unit = Unit::hertz,
          .midpoint = 141.0f },
        { .id = "highCut",
          .name = "High Cut",
          .minimum = 1000.0f,
          .maximum = 20000.0f,
          .defaultValue = 20000.0f,
          .unit = Unit::hertz,
          .midpoint = 4472.0f },
        { .id = "highCutSlope", .name = "High Cut Slope", .maximum = 1.0f, .defaultValue = 1.0f, .unit = Unit::choice, .choices = slopes },
        { .id = "width", .name = "Width", .defaultValue = 1.0f, .unit = Unit::percent },
        { .id = mixParameterID, .name = "Mix", .defaultValue = 0.25f, .unit = Unit::percent },
    };

    // The room's lines' lengths at the largest size, in milliseconds: spread apart, and sharing no common period.
    constexpr double lineMilliseconds[Reverb::numLines] = { 44.3, 49.8, 57.1, 63.4, 69.7, 74.3, 82.9, 89.5 };
    constexpr double smallest = 0.25; // of the largest lengths, at size 0: the room's lines', and the plate's

    // How far each line's length moves, and how fast: slowly, each at a rate of its own.
    constexpr double modulationMilliseconds = 0.12;
    constexpr double modulationHertz[Reverb::numLines] = { 0.31, 0.47, 0.53, 0.67, 0.79, 0.89, 1.01, 1.13 };
    constexpr std::uint32_t modulationSeed = 0x5eed1234u;

    constexpr float highCutOff = 19999.0f; // the high cut at its top: off

    // The allpass filters before the room's network, from Dattorro's plate (1997): their lengths in milliseconds.
    constexpr double diffuserMilliseconds[] = { 4.77, 3.59, 12.73, 9.30 };
    constexpr double rightSpread = 1.07; // the right channel's are a little longer, so the sides differ

    // The allpass filters' gains at the Diffusion's middle, the paper's (its table 1): the two pairs that smear what comes in,
    // which the room has before its lines too, and the two pairs in the plate's tank. Below the middle they all fall to
    // none. Above it, those the paper keeps lower rise to the most it uses, three quarters: the tail then comes denser, and
    // lengthens more as it fades.
    constexpr float inputGains[] = { 0.75f, 0.75f, 0.625f, 0.625f };
    constexpr float tankGain1 = 0.70f, tankGain2 = 0.50f;
    constexpr float fullGain = 0.75f;

    float getAllpassGain (float atMiddle, float diffusion)
    {
        return diffusion <= 0.5f ? atMiddle * (2.0f * diffusion) : atMiddle + (fullGain - atMiddle) * (2.0f * diffusion - 1.0f);
    }

    // Which lines each side hears, and how: the two patterns are orthogonal, so the sides are uncorrelated.
    constexpr float leftSigns[Reverb::numLines] = { 1, -1, 1, -1, 1, -1, 1, -1 };
    constexpr float rightSigns[Reverb::numLines] = { 1, 1, -1, -1, 1, 1, -1, -1 };
    constexpr float lineScale = 0.35355339f;  // 1 / sqrt (numLines): how much of each side goes into each of its lines
    constexpr float outputGain = 0.70710678f; // so that a 2 s tail of steady noise sits about 6 dB under the noise

    constexpr double smoothingSeconds = 0.1;  // how long the size and pre-delay take to follow their knobs, about
    constexpr double modeFadeSeconds = 0.03;  // a mode fading out, then the other in
    constexpr double slopeFadeSeconds = 0.01; // the high cut going from one slope to the other

    double getLineSamples (int line, float size, double sampleRate)
    {
        return lineMilliseconds[line] * 0.001 * sampleRate * (smallest + (1.0 - smallest) * size);
    }

    // The low-pass on each pass, y += c (x - y): its coefficient for a frequency, and how much it keeps of a frequency.
    double getDampingCoefficient (double frequency, double sampleRate)
    {
        return 1.0 - std::exp (-juce::MathConstants<double>::twoPi * frequency / sampleRate);
    }

    double getDampingMagnitude (double c, double frequency, double sampleRate)
    {
        const auto omega = juce::MathConstants<double>::twoPi * std::min (frequency, 0.5 * sampleRate) / sampleRate;
        return c / std::sqrt (1.0 - 2.0 * (1.0 - c) * std::cos (omega) + (1.0 - c) * (1.0 - c));
    }

    //==============================================================================
    // The plate: Dattorro's lengths, in samples at the paper's sample rate (its figure 1). The four allpass filters that
    // smear what comes in; then each half of the tank: its swaying allpass filter, a delay, an allpass filter, a delay.
    constexpr double paperRate = 29761.0;
    constexpr double plateDiffuserSamples[] = { 142.0, 107.0, 379.0, 277.0 };
    constexpr double tankSamples[2][4] = { { 672.0, 4453.0, 1800.0, 3720.0 }, { 908.0, 4217.0, 2656.0, 3163.0 } };
    constexpr double loopSeconds = (672.0 + 4453.0 + 1800.0 + 3720.0 + 908.0 + 4217.0 + 2656.0 + 3163.0) / paperRate; // once round
    constexpr double longestAllpassSeconds = 2656.0 / paperRate;

    // What each side hears (the paper's table 2): which half's which part, how many samples back, and its sign. A tap of an
    // allpass filter reads inside its line.
    struct Tap
    {
        int half, part;
        double samples;
        float sign;
    };

    // clang-format off
    constexpr Tap taps[2][7] = {
        { { 1, 1, 266.0, 1.0f }, { 1, 1, 2974.0, 1.0f }, { 1, 2, 1913.0, -1.0f }, { 1, 3, 1996.0, 1.0f },
          { 0, 1, 1990.0, -1.0f }, { 0, 2, 187.0, -1.0f }, { 0, 3, 1066.0, -1.0f } },
        { { 0, 1, 353.0, 1.0f }, { 0, 1, 3627.0, 1.0f }, { 0, 2, 1228.0, -1.0f }, { 0, 3, 2673.0, 1.0f },
          { 1, 1, 2111.0, -1.0f }, { 1, 2, 335.0, -1.0f }, { 1, 3, 121.0, -1.0f } },
    };
    // clang-format on

    constexpr float tapGain = 0.6f; // the paper's

    // So that the plate's tail is about as loud as the room's at the same settings as heard (pink noise through the ear's
    // weighting, ITU-R BS.1770's K curve: within a dB at the settings both start with and at the presets' rooms, halls and
    // plates), and a mode changed keeps the level: 1.4 dB under the paper's own level.
    constexpr float plateOutputGain = 0.85f;

    // The plate at size 1, of the paper's lengths; at the size's middle it is the paper's (1.6 x (0.25 + 0.75 x 0.5) = 1).
    constexpr double largestPlate = 1.6;

    // The tank's two swaying allpass filters: how far their lengths move, and how fast, a quarter turn apart. The paper
    // asks for a rate "on the order of 1 Hz" and 8 samples of its 29.8 kHz either way (0.27 ms), 16 at most; this is about
    // a pitch that wavers by 2.5 cents on each pass, enough to break up the tank's ringing, too little to hear as vibrato.
    constexpr double swayMilliseconds = 0.385, swayHertz = 0.6;

    // The plate's two damping filters, each a low-pass at this much of the Damping: two octaves under it. The room's sound
    // passes its damping some 24 times a second at the size's middle, losing 3 dB at the Damping frequency each time and
    // 1 dB an octave under it, where its treble starts to fade sooner; the plate's passes its filters 2.8 times a second, so
    // each has to take about 8 dB an octave under the Damping for its tail to lose as much there, which a low-pass two
    // octaves under it does (7 dB). Above that the room's tail darkens the faster, as it passes its filter more often. So
    // the Damping means about the same in both modes, at any size (the room's lines and the plate grow alike), and a mode
    // changed keeps the tail's colour where it starts to fade. Dattorro's own damping coefficients (and CAPS's) are this
    // filter's: a corner of 4.3 kHz is a Damping of 17.1 kHz.
    constexpr double plateDampingShare = 0.25;

    // The shortest decay a plate of the paper's size fades in: what comes out of the tank's first delays alone is that long.
    // A shorter decay shrinks the plate.
    constexpr double shortestLoopDecay = 0.6;

    constexpr float settledLength = 0.001f; // of a sample: a gliding length this near its place is there

    int getLengthOfDiffuser (int stage)
    {
        return stage;
    }

    int getLengthOfTankPart (int half, int part)
    {
        return 4 + 4 * half + part;
    }

    int getLengthOfTap (int side, int tap)
    {
        return 12 + 7 * side + tap;
    }

    double getPaperSamples (int length)
    {
        if (length < 4)
            return plateDiffuserSamples[length];

        if (length < 12)
            return tankSamples[(length - 4) / 4][(length - 4) % 4];

        return taps[(length - 12) / 7][(length - 12) % 7].samples;
    }

    // The plate's decay, measured: how long its sound below 1.5 kHz takes to fade by 60 dB (twice the time its energy takes
    // from -5 to -35 dB, as a room's is measured), over what its loop alone would give (its length once round, and the four
    // gains on the way: 3 x loopSeconds / (-4 log10 (gain))). Each row a Diffusion, from none to full in eighths; each
    // column a loop time, from a quarter of a second, each the square root of 2 times the one before, to 64 s. The allpass
    // filters hold the sound longer than the loop says, the more the more they smear and the shorter the loop time is
    // against their own ringing. Measured at 44.1 kHz at the paper's size, with the sway on; the tests measure the device
    // against its Decay at other sizes and sample rates.
    constexpr int numLoopTimes = 17, numDiffusions = 9;
    constexpr double firstLoopTime = 0.25;

    // clang-format off
    constexpr float decayRatios[numDiffusions][numLoopTimes] = {
        { 0.117f, 0.780f, 0.793f, 0.929f, 0.985f, 1.007f, 1.006f, 0.989f, 1.013f, 0.996f, 1.009f, 1.004f, 1.001f, 0.998f, 1.001f, 0.999f, 1.000f },
        { 0.510f, 0.854f, 0.805f, 0.952f, 0.985f, 1.007f, 1.006f, 0.998f, 1.013f, 1.005f, 1.007f, 1.006f, 1.003f, 1.002f, 1.003f, 1.003f, 1.003f },
        { 0.834f, 1.018f, 0.939f, 1.005f, 0.994f, 1.018f, 1.018f, 1.020f, 1.024f, 1.016f, 1.016f, 1.012f, 1.010f, 1.012f, 1.012f, 1.012f, 1.012f },
        { 1.264f, 1.112f, 1.094f, 1.070f, 1.031f, 1.048f, 1.041f, 1.040f, 1.038f, 1.028f, 1.035f, 1.025f, 1.027f, 1.027f, 1.024f, 1.021f, 1.022f },
        { 2.199f, 1.603f, 1.353f, 1.236f, 1.148f, 1.093f, 1.076f, 1.066f, 1.058f, 1.062f, 1.061f, 1.054f, 1.047f, 1.046f, 1.042f, 1.039f, 1.039f },
        { 2.291f, 1.711f, 1.479f, 1.326f, 1.217f, 1.132f, 1.103f, 1.088f, 1.084f, 1.083f, 1.081f, 1.076f, 1.068f, 1.065f, 1.060f, 1.058f, 1.055f },
        { 2.449f, 1.820f, 1.624f, 1.499f, 1.344f, 1.201f, 1.157f, 1.129f, 1.124f, 1.115f, 1.108f, 1.095f, 1.087f, 1.083f, 1.080f, 1.078f, 1.074f },
        { 2.595f, 1.975f, 1.850f, 1.773f, 1.537f, 1.315f, 1.238f, 1.196f, 1.181f, 1.170f, 1.152f, 1.132f, 1.121f, 1.112f, 1.110f, 1.108f, 1.108f },
        { 2.782f, 2.178f, 2.291f, 2.202f, 1.882f, 1.535f, 1.381f, 1.310f, 1.275f, 1.255f, 1.233f, 1.206f, 1.188f, 1.177f, 1.169f, 1.165f, 1.163f },
    };
    // clang-format on

    double getDecayRatio (double loopTime, double diffusion)
    {
        const auto column = std::clamp (2.0 * std::log2 (loopTime / firstLoopTime), 0.0, numLoopTimes - 1.0);
        const auto row = std::clamp (diffusion * (numDiffusions - 1.0), 0.0, numDiffusions - 1.0);
        const auto c = std::min ((int) column, numLoopTimes - 2), r = std::min ((int) row, numDiffusions - 2);
        const auto along = column - c, down = row - r;
        const auto above = decayRatios[r][c] + (decayRatios[r][c + 1] - decayRatios[r][c]) * along;
        const auto below = decayRatios[r + 1][c] + (decayRatios[r + 1][c + 1] - decayRatios[r + 1][c]) * along;
        return above + (below - above) * down;
    }

    // The loop time whose measured decay is the one asked for: the longer the loop time, the longer the decay, so halving
    // the range finds it.
    double findLoopTime (double loopDecay, double diffusion)
    {
        auto low = std::log (0.02), high = std::log (400.0);

        for (int step = 0; step < 36; ++step)
        {
            const auto middle = 0.5 * (low + high);
            const auto time = std::exp (middle);
            (time * getDecayRatio (time, diffusion) < loopDecay ? low : high) = middle;
        }

        return std::exp (0.5 * (low + high));
    }

    double getTankGain (double loopTime)
    {
        return std::pow (10.0, -0.75 * loopSeconds / loopTime);
    }
} // namespace

const DeviceInfo Reverb::info { .typeID = typeID, .name = "Reverb", .kind = DeviceKind::effect, .parameters = parameters };

Reverb::PlateSetting Reverb::getPlateSetting (std::span<const float> values)
{
    const auto seconds = (double) values[decay];
    PlateSetting setting;

    // As large as the Size says, but no larger than its sound can fade in.
    setting.scale = std::min (largestPlate * (smallest + (1.0 - smallest) * (double) values[size]), seconds / shortestLoopDecay);
    const auto loopDecay = seconds / setting.scale; // the decay a plate of the paper's size would need, to sound the same

    // As smeared as the Diffusion says, but no allpass filter rings longer than the tail: the longest of them fades by 60 dB
    // in 3 x its length / -log10 (its gain).
    const auto mostGain = std::pow (10.0, -3.0 * longestAllpassSeconds / loopDecay);
    const auto mostDiffusion = mostGain <= (double) tankGain2
                                   ? 0.5 * mostGain / (double) tankGain2
                                   : 0.5 + 0.5 * (mostGain - (double) tankGain2) / (double) (fullGain - tankGain2);
    setting.diffusion = std::clamp (std::min ((double) values[diffusion], mostDiffusion), 0.0, 1.0);
    setting.gain = getTankGain (findLoopTime (loopDecay, setting.diffusion));
    return setting;
}

double Reverb::getPlateSize (double scale)
{
    return std::clamp ((scale / largestPlate - smallest) / (1.0 - smallest), 0.0, 1.0);
}

double Reverb::getDecayAt (std::span<const float> values, double frequency, double sampleRate)
{
    const auto isPlate = values[mode] > 0.5f;
    const auto c = getDampingCoefficient (values[damping] * (isPlate ? plateDampingShare : 1.0), sampleRate);
    const auto magnitude = getDampingMagnitude (c, frequency, sampleRate);

    if (isPlate)
    {
        // Once round the tank: four gains and two dampings. The loop time that would give alone, and what the table makes
        // of it.
        const auto setting = getPlateSetting (values);
        const auto loopTime = -0.75 * loopSeconds / std::log10 (std::min (0.9999, setting.gain * std::sqrt (magnitude)));
        return setting.scale * loopTime * getDecayRatio (loopTime, setting.diffusion);
    }

    // A pass through an average line: the loss that makes the decay, and the damping filter's at this frequency.
    auto meanSamples = 0.0;

    for (int line = 0; line < numLines; ++line)
        meanSamples += getLineSamples (line, values[size], sampleRate) / numLines;

    const auto decibelsPerPass = 60.0 * meanSamples / ((double) values[decay] * sampleRate);
    const auto dampedDecibels = -20.0 * std::log10 (magnitude);
    return 60.0 * meanSamples / (sampleRate * (decibelsPerPass + dampedDecibels));
}

void Reverb::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    smoothing = (float) (1.0 - std::exp (-1.0 / (smoothingSeconds * sampleRate)));
    modeStep = (float) (1.0 / (modeFadeSeconds * sampleRate));
    gentleStep = (float) (1.0 / (slopeFadeSeconds * sampleRate));
    const auto modulationDepth = modulationMilliseconds * 0.001 * sampleRate;

    for (int line = 0; line < numLines; ++line)
    {
        lines[(size_t) line].prepare ((int) std::ceil (getLineSamples (line, 1.0f, sampleRate) + 2.0 * modulationDepth) + 2);
        phaseSteps[(size_t) line] = (float) (modulationHertz[line] / sampleRate);
    }

    for (auto& line : predelays)
        line.prepare ((int) std::ceil (0.001 * parameters[predelay].maximum * sampleRate) + 2);

    for (size_t side = 0; side < 2; ++side)
    {
        for (size_t stage = 0; stage < (size_t) numDiffusers; ++stage)
        {
            auto& allpass = diffusers[side][stage];
            allpass.length =
                std::max (1, (int) std::lround (diffuserMilliseconds[stage] * 0.001 * sampleRate * (side == 0 ? 1.0 : rightSpread)));
            allpass.gain = inputGains[stage];
            allpass.line.prepare (allpass.length);
        }
    }

    // The plate's lines, each long enough for the largest plate, and the swaying ones for their sway.
    plateSway = (float) (swayMilliseconds * 0.001 * sampleRate);
    platePhaseStep = (float) (swayHertz / sampleRate);
    const auto longest = [this] (double paperSamples)
    { return (int) std::ceil (paperSamples * sampleRate / paperRate * largestPlate + (double) plateSway) + 4; };

    for (size_t stage = 0; stage < (size_t) numDiffusers; ++stage)
        plateDiffusers[stage].prepare (longest (plateDiffuserSamples[stage]));

    for (size_t half = 0; half < 2; ++half)
        for (size_t part = 0; part < (size_t) numTankParts; ++part)
            tank[half][part].prepare (longest (tankSamples[half][part]));

    reset();
}

void Reverb::reset()
{
    for (auto& line : predelays)
        line.reset();

    for (auto& filter : lowCuts)
        filter.reset();

    for (auto& filter : highCuts)
        filter.reset();

    gentleCuts.fill (0.0f);
    gentleShare = -1.0f;
    resetRoom();
    resetPlate();
    playing = -1;
    modeLevel = 1.0f;
}

void Reverb::resetRoom()
{
    for (auto& line : lines)
        line.reset();

    for (auto& side : diffusers)
        for (auto& allpass : side)
            allpass.line.reset();

    damped.fill (0.0f);

    // The same starting points every time, so that a render comes out the same.
    dsp::Random random (modulationSeed);

    for (auto& phase : phases)
        phase = random.nextUnipolar();

    shownSize = -1.0f;
}

void Reverb::resetPlate()
{
    for (auto& line : plateDiffusers)
        line.reset();

    for (auto& half : tank)
        for (auto& line : half)
            line.reset();

    tankDamped.fill (0.0f);
    platePhase = 0.0f; // the same sway every time
    plateStarted = false;
}

double Reverb::getTailSeconds() const
{
    return (double) tailSeconds.load (std::memory_order_relaxed);
}

Reverb::Output Reverb::makeOutput (std::span<const float> values) const
{
    Output output;
    output.lowCut = dsp::makeFilter (dsp::FilterShape::highPass, values[lowCut], sampleRate, 0.70710678);
    output.cutsHigh = values[highCut] < highCutOff;
    output.highCut = dsp::makeFilter (dsp::FilterShape::lowPass, values[highCut], sampleRate, 0.70710678);
    output.isGentle = values[highCutSlope] < 0.5f;
    output.gentle = (float) getDampingCoefficient (values[highCut], sampleRate);
    output.spread = values[width];
    return output;
}

// The way out: as wide as the width, without rumble, and without the treble above the high cut.
inline void Reverb::putOut (float& left, float& right, const Output& output, float gain) noexcept
{
    const auto mid = 0.5f * (left + right), sideways = 0.5f * (left - right) * output.spread;
    left = lowCuts[0].process ((mid + sideways) * gain, output.lowCut);
    right = lowCuts[1].process ((mid - sideways) * gain, output.lowCut);

    // Both slopes' filters run while it cuts, and a slope changed glides from one's sound to the other's.
    if (output.cutsHigh)
    {
        const auto steepLeft = highCuts[0].process (left, output.highCut), steepRight = highCuts[1].process (right, output.highCut);
        const auto gentleLeft = gentleCuts[0] += (left - gentleCuts[0]) * output.gentle;
        const auto gentleRight = gentleCuts[1] += (right - gentleCuts[1]) * output.gentle;
        gentleShare = output.isGentle ? std::min (1.0f, gentleShare + gentleStep) : std::max (0.0f, gentleShare - gentleStep);

        if (gentleShare <= 0.0f)
        {
            left = steepLeft;
            right = steepRight;
        }
        else if (gentleShare >= 1.0f)
        {
            left = gentleLeft;
            right = gentleRight;
        }
        else
        {
            left = steepLeft + (gentleLeft - steepLeft) * gentleShare;
            right = steepRight + (gentleRight - steepRight) * gentleShare;
        }
    }
}

void Reverb::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);

    if (numChannels == 0)
        return;

    tailSeconds.store (values[decay] + values[predelay] * 0.001f, std::memory_order_relaxed);

    if (gentleShare < 0.0f)
        gentleShare = values[highCutSlope] < 0.5f ? 1.0f : 0.0f; // the slope it starts with, at once

    // The mode that plays: the one asked for, once the one before it has faded out; it starts empty.
    const auto wanted = values[mode] > 0.5f ? (int) plate : (int) room;

    if (playing < 0)
    {
        playing = wanted;
    }
    else if (playing != wanted && modeLevel <= 0.0f)
    {
        playing = wanted;

        if (playing == plate)
            resetPlate();
        else
            resetRoom();
    }

    // Out with one mode, then in with the other, each along a curve that starts and ends level, so that neither clicks. What
    // a mode is fed follows the curve too: a sound cut in at full level would be a click of its own, which it would then
    // reverberate; and a mode asked for again as it fades out comes back from where it was.
    const auto curve = [] (float level) { return level * level * (3.0f - 2.0f * level); };
    const auto fades = playing != wanted || modeLevel < 1.0f;
    const auto step = playing != wanted ? -modeStep : modeStep;
    feedFrom = fades ? curve (modeLevel) : 1.0f;
    feedTo = fades ? curve (std::clamp (modeLevel + step * (float) block.numSamples, 0.0f, 1.0f)) : 1.0f;

    if (playing == plate)
        processPlate (block, numChannels);
    else
        processRoom (block, numChannels);

    if (fades)
    {
        for (int i = 0; i < block.numSamples; ++i)
        {
            modeLevel = std::clamp (modeLevel + step, 0.0f, 1.0f);
            const auto gain = curve (modeLevel);

            for (int channel = 0; channel < numChannels; ++channel)
                block.channels[channel][i] *= gain;
        }
    }
}

void Reverb::processRoom (const ProcessContext& block, int numChannels)
{
    const auto& values = block.parameters;
    const auto decaySeconds = values[decay];
    const auto targetPredelay = (float) (values[predelay] * 0.001 * sampleRate);
    const auto dampingCoefficient = (float) getDampingCoefficient (values[damping], sampleRate);
    const auto output = makeOutput (values);
    const auto modulationDepth = (float) (modulationMilliseconds * 0.001 * sampleRate);

    if (shownSize < 0.0f)
    {
        shownSize = values[size];
        shownPredelay = targetPredelay;
        shownDiffusion = values[diffusion];
    }

    // Each line's loss on each pass, so that every line fades by 60 dB over the decay time; worked out for the size at
    // the block's start. And its damping: as much treble lost on a pass as its length is of the lines' mean, as its loss
    // is (Jot and Chaigne, 1991), so that a short line, which goes round more often, loses its treble no faster than a
    // long one and the tail keeps its colour as it fades. Matched at the Nyquist frequency, where a one-pole low-pass of
    // coefficient c keeps c / (2 - c); the mean line's is the damping knob's, which getDecayAt() draws.
    std::array<float, numLines> lengths {}, losses {}, dampings {};
    auto meanLength = 0.0f;

    for (int line = 0; line < numLines; ++line)
    {
        lengths[(size_t) line] = (float) getLineSamples (line, shownSize, sampleRate);
        losses[(size_t) line] = (float) std::pow (10.0, -3.0 * lengths[(size_t) line] / (decaySeconds * sampleRate));
        meanLength += lengths[(size_t) line] / (float) numLines;
    }

    const auto keptAtNyquist = (double) dampingCoefficient / (2.0 - (double) dampingCoefficient);

    for (int line = 0; line < numLines; ++line)
    {
        const auto kept = std::pow (keptAtNyquist, (double) (lengths[(size_t) line] / meanLength));
        dampings[(size_t) line] = (float) (2.0 * kept / (1.0 + kept));
    }

    const auto sizeStep = (values[size] - shownSize) * std::min (1.0f, smoothing * (float) block.numSamples);
    const auto lengthScale = [] (float shown) { return (float) (smallest + (1.0 - smallest) * shown); };
    const auto startScale = lengthScale (shownSize), endScale = lengthScale (shownSize + sizeStep);
    shownSize += sizeStep;

    // How far the allpass filters smear what comes in: the Diffusion, gliding to where it was turned over the block.
    const auto diffusionFrom = shownDiffusion, diffusionTo = values[diffusion];
    const auto diffusionMoves = ! juce::exactlyEqual (diffusionFrom, diffusionTo);
    shownDiffusion = diffusionTo;
    const auto setDiffusion = [this] (float amount)
    {
        for (auto& side : diffusers)
            for (size_t stage = 0; stage < (size_t) numDiffusers; ++stage)
                side[stage].gain = getAllpassGain (inputGains[stage], amount);
    };
    setDiffusion (diffusionFrom);

    for (int i = 0; i < block.numSamples; ++i)
    {
        const auto along = (float) (i + 1) / (float) block.numSamples;

        if (diffusionMoves)
            setDiffusion (diffusionFrom + (diffusionTo - diffusionFrom) * along);

        // In: delayed, then smeared, each side on its own.
        shownPredelay += (targetPredelay - shownPredelay) * smoothing;
        const auto feed = feedFrom + (feedTo - feedFrom) * along;
        std::array<float, 2> in {};

        for (size_t side = 0; side < 2; ++side)
        {
            const auto sample = block.channels[std::min ((int) side, numChannels - 1)][i];
            auto& line = predelays[side];
            line.push (sample);
            auto diffused = line.read (shownPredelay) * feed;

            for (auto& allpass : diffusers[side])
                diffused = allpass.process (diffused);

            in[side] = diffused;
        }

        // The network: each line's output, modulated, damped and faded, mixed into every line's input.
        const auto scale = startScale + (endScale - startScale) * along;
        std::array<float, numLines> out {};
        auto sum = 0.0f;

        for (size_t line = 0; line < (size_t) numLines; ++line)
        {
            auto& phase = phases[line];
            phase += phaseSteps[line];
            phase -= std::floor (phase);

            const auto delay = lengths[line] / startScale * scale + modulationDepth * (1.0f + dsp::sineOfPhase (phase));
            auto& smooth = damped[line];
            smooth += (lines[line].read (delay) - smooth) * dampings[line];
            out[line] = smooth * losses[line];
            sum += out[line];
        }

        // A Householder matrix: each line gets its own output less a quarter of them all, which keeps the energy.
        const auto shared = sum * (2.0f / (float) numLines);
        auto left = 0.0f, right = 0.0f;

        for (size_t line = 0; line < (size_t) numLines; ++line)
        {
            lines[line].push (out[line] - shared + in[line % 2] * lineScale);
            left += leftSigns[line] * out[line];
            right += rightSigns[line] * out[line];
        }

        putOut (left, right, output, outputGain);
        block.channels[0][i] = left;

        if (numChannels > 1)
            block.channels[1][i] = right;
    }
}

template <bool glides>
void Reverb::playPlate (const ProcessContext& block, int numChannels, const PlateValues& from, const PlateValues& to,
                        const std::array<float, numPlateLengths>& lengthSteps)
{
    const auto& values = block.parameters;
    const auto targetPredelay = (float) (values[predelay] * 0.001 * sampleRate);
    const auto output = makeOutput (values);
    auto& lengths = plateLengths;
    std::array<int, numPlateLengths> whole {};

    for (size_t length = 0; length < (size_t) numPlateLengths; ++length)
        whole[length] = (int) lengths[length];

    // A line's sample from as far back as one of the lengths says, before this sample is pushed (so one less back); and a
    // tap's, after.
    const auto back = [&] (const dsp::DelayLine& line, int length)
    {
        if constexpr (glides)
            return line.read (lengths[(size_t) length] - 1.0f);
        else
            return line.read (whole[(size_t) length] - 1);
    };
    const auto tapped = [&] (const dsp::DelayLine& line, int length)
    {
        if constexpr (glides)
            return line.read (lengths[(size_t) length]);
        else
            return line.read (whole[(size_t) length]);
    };

    for (int i = 0; i < block.numSamples; ++i)
    {
        const auto along = (float) (i + 1) / (float) block.numSamples;
        const auto gain = from.gain + (to.gain - from.gain) * along;
        const auto dampingNow = from.damping + (to.damping - from.damping) * along;
        const std::array inputGain { from.input1 + (to.input1 - from.input1) * along, from.input2 + (to.input2 - from.input2) * along };
        const auto tank1 = from.tank1 + (to.tank1 - from.tank1) * along;
        const auto tank2 = from.tank2 + (to.tank2 - from.tank2) * along;

        if constexpr (glides)
            for (size_t length = 0; length < (size_t) numPlateLengths; ++length)
                lengths[length] += lengthSteps[length];

        // In: both sides as one, as a plate has one driver; delayed, then smeared by four allpass filters.
        shownPredelay += (targetPredelay - shownPredelay) * smoothing;
        auto in = 0.0f;

        for (size_t side = 0; side < 2; ++side)
        {
            auto& line = predelays[side];
            line.push (block.channels[std::min ((int) side, numChannels - 1)][i]);
            in += line.read (shownPredelay);
        }

        in *= 0.5f * (feedFrom + (feedTo - feedFrom) * along);

        for (int stage = 0; stage < numDiffusers; ++stage)
        {
            auto& line = plateDiffusers[(size_t) stage];
            const auto k = inputGain[(size_t) stage / 2];
            const auto delayed = back (line, getLengthOfDiffuser (stage));
            const auto fed = in - k * delayed;
            line.push (fed);
            in = delayed + k * fed;
        }

        // The tank: each half takes what came in and what the other half ends with, faded.
        platePhase += platePhaseStep;
        platePhase -= std::floor (platePhase);
        const std::array ends { back (tank[0][3], getLengthOfTankPart (0, 3)), back (tank[1][3], getLengthOfTankPart (1, 3)) };

        for (int half = 0; half < 2; ++half)
        {
            auto& parts = tank[(size_t) half];

            // The allpass filter whose length sways, its gain's sign turned (the paper's "note sign"), a quarter turn from
            // the other half's.
            const auto sway = plateSway * dsp::sineOfPhase (platePhase + (half == 0 ? 0.0f : 0.25f));
            const auto swayed = parts[0].read (lengths[(size_t) getLengthOfTankPart (half, 0)] - 1.0f + sway);
            const auto fed = in + gain * ends[(size_t) (1 - half)] + tank1 * swayed;
            parts[0].push (fed);

            // A delay; then the damping and the decay's gain.
            const auto delayed = back (parts[1], getLengthOfTankPart (half, 1));
            parts[1].push (swayed - tank1 * fed);
            auto& smooth = tankDamped[(size_t) half];
            smooth += (delayed - smooth) * dampingNow;

            // An allpass filter, and a delay, whose end the other half takes.
            const auto held = back (parts[2], getLengthOfTankPart (half, 2));
            const auto into = smooth * gain - tank2 * held;
            parts[2].push (into);
            parts[3].push (held + tank2 * into);
        }

        // Out: each side its seven taps of the tank.
        std::array<float, 2> out {};

        for (int side = 0; side < 2; ++side)
        {
            for (int n = 0; n < numTaps; ++n)
            {
                const auto& tap = taps[side][n];
                out[(size_t) side] += tap.sign * tapped (tank[(size_t) tap.half][(size_t) tap.part], getLengthOfTap (side, n));
            }
        }

        auto left = out[0] * tapGain, right = out[1] * tapGain;
        putOut (left, right, output, plateOutputGain);
        block.channels[0][i] = left;

        if (numChannels > 1)
            block.channels[1][i] = right;
    }
}

void Reverb::processPlate (const ProcessContext& block, int numChannels)
{
    const auto& values = block.parameters;

    if (! juce::exactlyEqual (values[size], plateSettingFor[0]) || ! juce::exactlyEqual (values[decay], plateSettingFor[1])
        || ! juce::exactlyEqual (values[diffusion], plateSettingFor[2]))
    {
        plateSetting = getPlateSetting (values);
        plateSettingFor = { values[size], values[decay], values[diffusion] };
    }

    // What the sound goes through, gliding there over the block from where the last one left it.
    const auto smear = (float) plateSetting.diffusion;
    PlateValues to;
    to.gain = (float) plateSetting.gain;
    to.damping = (float) getDampingCoefficient (values[damping] * plateDampingShare, sampleRate);
    to.input1 = getAllpassGain (inputGains[0], smear);
    to.input2 = getAllpassGain (inputGains[2], smear);
    to.tank1 = getAllpassGain (tankGain1, smear);
    to.tank2 = getAllpassGain (tankGain2, smear);

    // Its lengths: whole numbers of samples, so that only the swaying allpass filters read between samples and the others
    // keep their treble however long the tail. A length whose place moves (the Size turned) glides there, as the room's
    // lines do, and the plate reads between samples meanwhile; it glides after an aim that glides to the place itself, so
    // that it sets off and arrives gently, and what the tank holds bends in pitch without a corner.
    const auto samplesPerPaperSample = sampleRate / paperRate * plateSetting.scale;
    const auto leastSwaying = std::ceil ((double) plateSway) + 3.0; // a swaying allpass filter reads no nearer than a sample back
    const auto share = std::min (1.0f, smoothing * (float) block.numSamples);
    std::array<float, numPlateLengths> steps {};
    auto glides = false;

    for (int length = 0; length < numPlateLengths; ++length)
    {
        const auto swaying = length == getLengthOfTankPart (0, 0) || length == getLengthOfTankPart (1, 0);
        const auto place = (float) std::max (swaying ? leastSwaying : 2.0, std::round (getPaperSamples (length) * samplesPerPaperSample));
        auto& shown = plateLengths[(size_t) length];
        auto& aim = plateAims[(size_t) length];

        if (! plateStarted)
        {
            shown = aim = place;
            continue;
        }

        aim = std::abs (place - aim) < settledLength ? place : aim + (place - aim) * share;

        if (juce::exactlyEqual (aim, place) && std::abs (place - shown) < settledLength)
        {
            shown = place;
        }
        else
        {
            steps[(size_t) length] = (aim - shown) * share / (float) block.numSamples;
            glides = true;
        }
    }

    if (! plateStarted)
    {
        plateValues = to;
        shownPredelay = (float) (values[predelay] * 0.001 * sampleRate);
        plateStarted = true;
    }

    const auto from = plateValues;
    plateValues = to;

    if (glides)
        playPlate<true> (block, numChannels, from, to, steps);
    else
        playPlate<false> (block, numChannels, from, to, steps);
}
} // namespace phazite::devices
