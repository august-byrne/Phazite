// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's multiband, in the layout and with the abilities of FL Studio's Maximus (the owner, 2026-11-01: "a single effect
// called multiband ... basically the same as FL Studio maximus. i want the same layout, the same abilities"). The sound,
// through a low cut, splits into low, mid and high bands at two crossovers of 12 or 24 dB per octave (Linkwitz-Riley, which
// add back up flat). Each band goes through its stereo separation, a pre gain, a compressor whose curve is drawn (level in
// against level out, through up to eight points, each line bent by a tension or shaped as an S, a hold or stairs), a post
// gain and a saturation with a ceiling. The bands, mixed with the sound as it came in (LMH Mix), go through the master band's
// same chain. Each compressor hears its band's peak, held for the sustain, or its average over the sustain (RMS); grips
// with an attack, the curve box rounding its start; and lets go through two releases, one speeding up (Release) and one
// slowing down (Release 2), which together make an S; and looks ahead, the bands by LMH Delay and the master by its attack,
// up to 5 ms each, so that a band can limit. Its latency is always the most look-ahead, 10 ms. Each band is on, its
// compression off, muted, or off (passing as it came, the mid band taking a low or high band's room), and a band can be
// soloed; the master's compressor can listen to the mid band instead of what it plays (Maximus's Master Mid mode: ducking,
// gating and de-essing), the low and high bands' compression off meanwhile. Not built: Maximus's oversampling, its
// linear-phase crossovers and its spare state (Phazite's undo and presets do that).
//
// The curves' axes: an amplitude a (1 at 0 dB) sits at ln (1 + 2a) / ln 9 across, from silence at 0 through 0 dB at the
// middle to +12 dB at 1, as Maximus draws its curves.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/MeterValue.h"
#include "devices/core/Processor.h"
#include "devices/dsp/DelayLine.h"
#include "devices/dsp/Filters.h"

#include <array>

namespace phazite::devices
{
class Multiband final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteMultiband";

    enum Band
    {
        low,
        mid,
        high,
        master,
        numBands
    };

    static constexpr int maxPoints = 8;

    // The global parameters, first; then each band's, in the bands' order (getBandParameter()), its points last
    // (getPointParameter()).
    enum Global
    {
        lowCut,
        lowFrequency,  // between the low and mid bands
        highFrequency, // between the mid and high bands
        lowSlope,      // 12 or 24 dB per octave
        highSlope,
        lmhDelay, // how far the bands look ahead
        lmhMix,   // how much of the bands, against the sound as it came in, goes to the master band
        midDrivesMaster,
        numGlobals
    };

    enum BandField
    {
        mode, // a Mode
        solo,
        stereo, // from -1 (mono) through 0 (as it is) to 1 (twice as wide)
        pre,    // decibels into the compressor
        post,   // decibels after it
        attack,
        release,      // speeding up
        release2,     // slowing down
        sustain,      // what the detector holds, or averages over
        curve,        // 1 to 8: how sharply the attack and the second release start (1) or how roundly (8)
        releaseCurve, // 1 to 8: how far the first release speeds up
        detection,    // peak or RMS
        saturation,   // from -1 (type A, softest at -1) through 0 (none) to 1 (type B)
        ceiling,      // decibels, the most the saturation lets out
        points,       // how many points its curve has, 2 to maxPoints
        firstPoint,
        bandFields = firstPoint + 4 * maxPoints
    };

    enum PointField
    {
        pointX, // across the curve's axis, from 0 to 1 (the first point at 0, the last at 1)
        pointY,
        pointTension, // from -1 to 1: how the line to it bends (up for 1)
        pointType,    // a PointType: how the line to it goes
        pointFields
    };

    enum class Mode
    {
        on,
        compressionOff, // everything but the compressor
        muted,
        off // the band passes as it came in; a low or high band off goes into the mid band
    };

    enum class PointType
    {
        singleCurve, // bent by its tension
        doubleCurve, // an S, bent by its tension
        hold,        // the level before it held until it
        stairs,      // steps, as many as its tension says
        smoothStairs // the same, each step rounded
    };

    static constexpr int getBandParameter (int band, BandField field) noexcept { return numGlobals + band * bandFields + field; }
    static constexpr int getPointParameter (int band, int point, PointField field) noexcept
    {
        return getBandParameter (band, firstPoint) + point * pointFields + field;
    }

    static const DeviceInfo info;

    // The curves' axis: an amplitude's place across it, from 0 to 1, and back; and where a level in decibels is.
    static float toAxis (float amplitude) noexcept;
    static float fromAxis (float place) noexcept;
    static float axisForDecibels (float decibels) noexcept;

    // A band's curve at a place across its axis (from 0 to 1), for the parameters' values in the device's order: where it
    // puts out, from 0 to 1. For its editor's drawing and for tests.
    static float getCurveAt (std::span<const float> parameters, int band, float place);

    // How many steps a stairs line takes, for a tension.
    static int getStairs (float tension) noexcept;

    // Each band as its editor shows it, since it last looked (devices/core/MeterValue.h), in decibels: what came in (after its
    // pre gain), what went out, what its compressor heard, and how far it was turned down (0 or more; up, below 0); and what
    // came into the device.
    MeterValue& getInputMeter (int band) noexcept { return inputMeters[(size_t) band]; }
    MeterValue& getOutputMeter (int band) noexcept { return outputMeters[(size_t) band]; }
    MeterValue& getHeardMeter (int band) noexcept { return heardMeters[(size_t) band]; }
    MeterValue& getReductionMeter (int band) noexcept { return reductionMeters[(size_t) band]; }
    MeterValue& getMainInputMeter() noexcept { return mainInputMeter; }

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    int getLatencySamples() const override { return 2 * lookahead; }
    const FallingValue* getGainReduction() const noexcept override { return &gainReduction; } // the master's

    // How far each part looks ahead, at most, in seconds; and how many entries a band's table of gains has.
    static constexpr double mostLookaheadSeconds = 0.005;
    static constexpr int tableSize = 512;

private:
    // One band's compressor: its table of gains along the axis, its detector and its timing.
    struct Compressor
    {
        std::array<float, tableSize + 1> gains {};          // decibels, for each place across the axis
        std::array<float, 1 + 4 * maxPoints> shownCurve {}; // the parameters the table was made from
        bool hasTable = false;
        float lastGain = 0.0f; // decibels: the amplitude's at the axis's end, for levels past it

        float meanSquare = 0.0f, held = 0.0f;
        int heldFor = 0;
        dsp::DelayLine heard; // its detector's level, to look ahead by delaying it less than the sound
        std::array<float, 8> attackStages {}, releaseStages {};
        float ramp = 0.0f, releaseFrom = 0.0f, gain = 0.0f; // decibels
        int releasedFor = -1;                               // samples since it began letting go, or -1 while it grips
        float linearGain = 1.0f, shownGain = 0.0f;
    };

    double sampleRate = 44100.0;
    int lookahead = 1;
    float preSmoothing = 0.001f;

    // A crossover's bands add up to what went in through an all-pass of the crossover's order: of a 24 dB crossover, second
    // (its coefficients given); of a 12 dB one, first (its coefficient given). What else meets the bands goes through the same,
    // so that they meet in phase.
    struct PhaseMatch
    {
        dsp::Filter secondOrder;
        float lastIn = 0.0f, lastOut = 0.0f; // the first-order one's

        float process (float in, bool steep, const dsp::FilterCoefficients& coefficients, float coefficient) noexcept
        {
            if (steep)
                return secondOrder.process (in, coefficients);

            const auto out = coefficient * in + lastIn - coefficient * lastOut;
            lastIn = in;
            lastOut = out;
            return out;
        }

        void reset() noexcept
        {
            secondOrder.reset();
            lastIn = lastOut = 0.0f;
        }
    };

    std::array<dsp::Filter, 2> lowCuts {};                                      // 12 dB per octave, per channel
    std::array<std::array<std::array<dsp::Filter, 2>, 2>, 2> lows {}, highs {}; // each crossover's, per channel, two sections
    std::array<PhaseMatch, 2> lowPhases {};                                     // the low band's around the second crossover, per channel
    std::array<std::array<PhaseMatch, 2>, 2> dryPhases {}; // the dry sound's (for LMH Mix) around each crossover, per channel
    std::array<dsp::DelayLine, 2> dryDelays, masterDelays;
    std::array<std::array<dsp::DelayLine, 2>, 3> bandDelays;
    std::array<Compressor, numBands> compressors;
    std::array<float, numBands> preGains {}, postGains {}; // gliding
    std::array<MeterValue, numBands> inputMeters { MeterValue { -120.0f }, MeterValue { -120.0f }, MeterValue { -120.0f },
                                                   MeterValue { -120.0f } };
    std::array<MeterValue, numBands> outputMeters { MeterValue { -120.0f }, MeterValue { -120.0f }, MeterValue { -120.0f },
                                                    MeterValue { -120.0f } };
    std::array<MeterValue, numBands> heardMeters { MeterValue { -120.0f }, MeterValue { -120.0f }, MeterValue { -120.0f },
                                                   MeterValue { -120.0f } };
    std::array<MeterValue, numBands> reductionMeters { MeterValue { -120.0f }, MeterValue { -120.0f }, MeterValue { -120.0f },
                                                       MeterValue { -120.0f } };
    MeterValue mainInputMeter { -120.0f };
    FallingValue gainReduction { reductionFallPerSecond };
    bool isFirstBlock = true;

    void updateCurve (Compressor&, std::span<const float> values, int band);
    float compress (Compressor&, std::span<const float> values, int band, float level, bool compresses) noexcept;
};
} // namespace phazite::devices
