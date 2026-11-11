// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's multiband, in the layout and with the abilities of FL Studio's Maximus (the owner, 2026-11-01: "a single effect
// called multiband ... basically the same as FL Studio maximus. i want the same layout, the same abilities"). The sound,
// through a low cut, splits into low, mid and high bands at two crossovers of 12 or 24 dB per octave (Linkwitz-Riley, which
// add back up flat). Each band goes through its stereo separation, a pre gain, a compressor whose curve is drawn (level in
// against level out, through up to eight points, each line bent by a tension or shaped as an S, a hold or stairs), a post
// gain, which is the band's level (the master's, what comes out), and a saturation with a ceiling. The bands, mixed with the
// sound as it came in (LMH Mix), go through the master band's same chain. Each compressor hears its band's peak, held for the
// sustain, or its average over the sustain (RMS); grips with an attack, the curve box rounding its start; and lets go through
// two releases, one speeding up (Release) and one slowing down (Release 2), which together make an S; and looks ahead, the
// bands by LMH Delay and the master by its attack, up to 5 ms each, so that a band can limit. Each saturation runs at twice
// the sample rate, played by its antiderivative (dsp/Oversampling.h), so that what it adds doesn't fold back as aliasing:
// while any of the three bands saturates, all of them and the sound mixed back in go that way, so that they stay in step;
// while none does, they take a plain delay as long, which leaves a limited peak where it is. Its latency is always the most
// look-ahead, 10 ms, and that of the two saturations in a row (the bands' and the master's). Each band is on, its
// compression off, muted, or off (passing as it came, the mid band taking a low or high band's room), and the low, mid and
// high bands can be soloed; the master's compressor can listen to the mid band instead of what it plays (Maximus's Master
// Mid mode: ducking, gating and de-essing), the low and high bands' compression off meanwhile.
//
// It has a sidechain (the owner, 2026-11-10: "sidechaining should be available for the compressor and the multiband"), as
// FabFilter Pro-MB keys each band from outside: its key is another track's sound (or, with none, what comes in), through a
// low cut and a high cut, as the compressor's key is; a band whose Key is on (any of the four) has its compressor hear the
// key instead of its own band (the master's, instead of the mid band too), so that only the lows duck under a kick, say.
// While its editor asks, it plays the key, as the bands hear it, instead of what it puts out (Listen).
//
// Not built: Maximus's oversampling of the whole chain, its linear-phase crossovers and its spare state (Phazite's undo and
// presets do that). Its curves are Maximus's, as the compressor's is (DynamicsCurve.h).
#pragma once

#include "DynamicsCurve.h"
#include "devices/core/DeviceInfo.h"
#include "devices/core/MeterValue.h"
#include "devices/core/Processor.h"
#include "devices/dsp/DelayLine.h"
#include "devices/dsp/Filters.h"
#include "devices/dsp/Oversampling.h"

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>

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
        lowSolo, // the low, mid and high bands soloed (getSoloParameter()); the master has none
        midSolo,
        highSolo,
        keyLowCut,  // the key's low cut (12 dB per octave),
        keyHighCut, // and its high cut (24 dB per octave, off at its top)
        numGlobals
    };

    enum BandField
    {
        mode,   // a Mode
        stereo, // from -1 (mono) through 0 (as it is) to 1 (twice as wide)
        pre,    // decibels into the compressor
        post,   // decibels after it: the band's level (the master's, what comes out)
        attack,
        release,      // speeding up
        release2,     // slowing down
        sustain,      // what the detector holds, or averages over
        curve,        // 1 to 8: how sharply the attack and the second release start (1) or how roundly (8)
        releaseCurve, // 1 to 8: how far the first release speeds up
        detection,    // peak or RMS
        saturation,   // from -1 (type A, softest at -1) through 0 (none) to 1 (type B)
        ceiling,      // decibels, the most the saturation lets out
        key,          // whether its compressor hears the key instead of its band
        points,       // its curve (DynamicsCurve.h): how many points it has, then its points
        bandFields = points + curves::parameterCount
    };

    enum class Mode
    {
        on,
        compressionOff, // everything but the compressor
        muted,
        off // the band passes as it came in; a low or high band off goes into the mid band
    };

    static constexpr int getBandParameter (int band, BandField field) noexcept { return numGlobals + band * bandFields + field; }
    static constexpr int getPointParameter (int band, int point, curves::PointField field) noexcept
    {
        return getBandParameter (band, points) + curves::getPointParameter (point, field);
    }

    // The low, mid or high band's solo.
    static constexpr int getSoloParameter (int band) noexcept { return lowSolo + band; }

    // A band's curve, among the parameters' values in the device's order.
    static std::span<const float> getCurve (std::span<const float> parameters, int band)
    {
        return parameters.subspan ((size_t) getBandParameter (band, points), curves::parameterCount);
    }

    static const DeviceInfo info;

    // Each band as its editor shows it, since it last looked (devices/core/MeterValue.h), in decibels: what came in (after its
    // pre gain), what went out, what its compressor heard (its band, or the key while its Key is on), and how far it was
    // turned down (0 or more; up, below 0); and what came into the device.
    MeterValue& getInputMeter (int band) noexcept { return inputMeters[(size_t) band]; }
    MeterValue& getOutputMeter (int band) noexcept { return outputMeters[(size_t) band]; }
    MeterValue& getHeardMeter (int band) noexcept { return heardMeters[(size_t) band]; }
    MeterValue& getReductionMeter (int band) noexcept { return reductionMeters[(size_t) band]; }
    MeterValue& getMainInputMeter() noexcept { return mainInputMeter; }

    // Message thread: plays the key, as the bands whose Key is on hear it (through its cuts), instead of what it puts out, for
    // the next half second by the clock; its editor asks again each time it refreshes, so that the key stops when the editor
    // no longer shows, as the compressor's Listen does. It fades between the two over a few milliseconds, the key as late as
    // what it puts out.
    void listenToKey() noexcept
    {
        listenUntil.store (juce::Time::getMillisecondCounterHiRes() + listenMilliseconds, std::memory_order_relaxed);
    }

    bool isPlayingKey() const noexcept { return juce::Time::getMillisecondCounterHiRes() < listenUntil.load (std::memory_order_relaxed); }
    bool overridesMix() const noexcept override { return keyShare > 0.0f; } // the block it last played: the key, or fading

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    int getLatencySamples() const override { return 2 * lookahead + 2 * saturationLatency; }
    const FallingValue* getGainReduction() const noexcept override { return &gainReduction; } // the master's

    // How far each part looks ahead, at most, in seconds.
    static constexpr double mostLookaheadSeconds = 0.005;

private:
    // One band's compressor: its curve's gains, its detector and its timing.
    struct Compressor
    {
        curves::Gains gains;

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
    int saturationLatency = 0; // a saturation's, rounded
    float preSmoothing = 0.001f;

    // A band's saturation as its antiderivative plays it: straight up to its threshold, then rounding off into its ceiling.
    struct SaturationCurve
    {
        float threshold = 1.0f, room = 0.0f; // room is the ceiling less the threshold: none for a straight line
        bool soft = true;                    // tanh's (type A) rather than the cubic's (type B)

        float value (float x) const noexcept;
        double integral (double x) const noexcept;
        bool isSame (const SaturationCurve&) const noexcept;
    };

    std::array<std::array<dsp::AntialiasedShaper, 2>, numBands> saturators; // each band's, per channel
    std::array<dsp::AntialiasedShaper, 2> dryInStep;                        // the sound mixed back in, kept in step with them
    std::array<SaturationCurve, numBands> lastCurves {};
    bool bandsSaturated = false, masterSaturated = false;    // whether the bands, and the master, went that way last
    std::array<dsp::DelayLine, 2> bandsInStep, masterInStep; // as long as a saturation, for while none saturates

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

    // The key: its cuts, per channel (the high cut's two sections), and as late as what it puts out, for Listen.
    std::array<dsp::Filter, 2> keyLowCuts {};
    std::array<std::array<dsp::Filter, 2>, 2> keyHighCuts {};
    std::array<dsp::DelayLine, 2> keyDelays;
    static constexpr double listenMilliseconds = 500.0;
    float keyShare = 0.0f, keyStep = 0.01f;  // how much of the key it plays, from 0 to 1, and its step per sample
    std::atomic<double> listenUntil { 0.0 }; // in the clock's milliseconds
    bool keyRunning = false;                 // whether the last block worked the key out

    static_assert (std::atomic<double>::is_always_lock_free);

    float compress (Compressor&, std::span<const float> values, int band, float level, bool compresses) noexcept;
    void resetKey() noexcept;
};
} // namespace phazite::devices
