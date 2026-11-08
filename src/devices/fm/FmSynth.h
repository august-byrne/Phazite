// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's FM synth (roadmap step 4.7; docs/design.md, section 9), "FM": four operators, as Ableton's Operator, Yamaha's
// reface DX and Elektron's Digitone have, each a sine (or a band-limited triangle, saw or square) at a ratio of the note's
// pitch, detuned in cents, or at a fixed frequency, with an envelope of its own, a level that velocity and the key scale,
// feedback of its own wave into itself (toward a saw one way, toward a square the other, as the reface DX's), and its place
// in one of eight algorithms, Yamaha's 4-operator ones: which operators modulate which, and which are heard (the carriers),
// from a stack of four to four side by side. Feedback is averaged over the last two samples, as the DX7 does, so that it
// doesn't hunt. Phase modulation, as the DX7 and every FM synth since do it, at twice the sample rate, so that what the
// operators make above hearing doesn't fold back down as aliasing. Then a pitch envelope (for drums and plucks), envelopes
// that run faster up the keys (Key Time, the DX7's rate scaling), the phases reset at each note or running free, a
// resonant filter of any of the types of dsp/MultimodeFilter.h, one LFO (to the pitch, the modulators' levels, the level or
// the cutoff), Timbre, which turns every modulator and every operator's feedback up or down at once, Spread, which plays
// each note twice, detuned apart, on the left and on the right (as Operator's Spread), and 16 voices, or one in mono mode,
// legato, with glide. A new synth plays a soft electric piano, two pairs built as the DX7's E.PIANO 1 is, so that it is
// finished from the start.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/LiveValue.h"
#include "devices/core/Processor.h"
#include "devices/dsp/Envelopes.h"
#include "devices/dsp/MultimodeFilter.h"
#include "devices/dsp/Oscillators.h"
#include "devices/dsp/Oversampling.h"
#include "devices/wavetable/Wavetable.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace phazite::devices
{
class FmSynth final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteFM";
    static constexpr int numOperators = 4, numAlgorithms = 8, maxVoices = 16;

    // An operator's parameters, in this order: operator op's (from 0) are from getParameterIndex (op, on) on.
    enum Field
    {
        on,
        wave,     // a Wave
        feedback, // its own wave fed back into it, from -1 to 1: above 0 toward a saw, below 0 toward a square; ±1 the DX7's 7
        ratio,
        fine,   // added to the ratio, less than the ratio's step, so that each ratio is set one way
        detune, // in cents, as two operators a few cents apart beat
        fixed,
        frequency, // while fixed, in hertz
        level,     // a carrier's loudness, a modulator's depth
        velocity,  // how much velocity changes its level
        keyScale,  // how much its level falls with each octave up from middle C (rises below it), or rises if below 0
        attack,
        decay,
        sustain,
        release,
        attackCurve, // how each stage bends (dsp::Adsr::setCurves())
        decayCurve,
        releaseCurve
    };

    static constexpr int fieldsPerOperator = 18;
    static constexpr int firstOperatorParameter = 2;

    static constexpr int getParameterIndex (int op, Field field) noexcept
    {
        return firstOperatorParameter + op * fieldsPerOperator + (int) field;
    }

    // The parameters, in the order of the description's, as the editor shows them; the operators' between timbre and
    // pitchAmount.
    enum Parameter
    {
        algorithm,
        timbre, // every modulator's level and every operator's feedback, from none through as set (100 %) to twice as deep
        pitchAmount = firstOperatorParameter + numOperators * fieldsPerOperator,
        pitchDecay,
        keyTime, // how much shorter the envelopes are each octave up from middle C (longer below it): at full, half
        phase,   // a Phase
        filterOn,
        filterType, // a dsp::FilterMode
        cutoff,
        resonance,
        filterSlope,
        keyTracking,
        lfoOn,
        lfoShape,
        lfoTarget,
        lfoRate,
        lfoDepth,
        voiceMode,
        glide,
        bendRange,
        spread, // in cents: above 0, each note plays twice, that far apart, the lower on the left and the higher on the right
        outputLevel
    };

    enum class Wave
    {
        sine,
        triangle,
        saw,
        square
    };

    enum class LfoTarget
    {
        pitch,  // up to a semitone either way
        timbre, // every modulator's level and every operator's feedback
        level,  // tremolo
        cutoff  // up to two octaves either way
    };

    enum class Phase
    {
        reset, // each note's operators from the start of their waves, the same every time: tight basses, plucks
        free   // running on from where they were, a little different each time, as acoustic sounds are
    };

    // An algorithm: for each operator, the operators that modulate it (bit j for operator j), and the operators heard.
    // Operators are counted from 0 (operator 1); each is only ever modulated by higher ones, so they are worked out from the
    // highest down.
    struct Algorithm
    {
        std::array<std::uint8_t, numOperators> modulatedBy {};
        std::uint8_t carriers = 0;
    };

    static const Algorithm& getAlgorithm (int index) noexcept;
    static bool isCarrier (int algorithm, int op) noexcept { return (getAlgorithm (algorithm).carriers >> op) & 1; }

    // The operators an operator modulates in an algorithm (bit j for operator j): none for a carrier.
    static std::uint8_t getTargets (int algorithm, int op) noexcept;

    static const DeviceInfo info;

    FmSynth();

    // How many voices are sounding, for tests and meters. Any thread.
    int getActiveVoices() const noexcept { return activeVoices.load (std::memory_order_relaxed); }

    // What its editor shows playing, once a block (devices/core/LiveValue.h): the newest voice's operators' envelopes (from
    // 0 to 1), the cutoff it plays (in hertz; 0 while no voice plays or the filter is off), and where the LFO is in its
    // cycle (0 to 1).
    struct Shown
    {
        std::array<LiveValue, numOperators> envelopes;
        LiveValue cutoff, lfoPhase;
    };

    const Shown& getShown() const noexcept { return shown; }

    // The filter's response at a frequency, as a gain, for the parameters' values with its cutoff at a frequency: for its
    // editor.
    static double getFilterMagnitude (std::span<const float> parameters, double cutoffHertz, double frequency, double sampleRate);

    // A modulator's depth in radians for its level (from 0 to 1), and a carrier's gain: both the square of the level, so
    // that the knob turns evenly to the ear, a modulator's up to 4 pi.
    static float getModulationIndex (float level) noexcept;
    static float getCarrierGain (float level) noexcept { return level * level; }

    // What the operators play for a note at middle C, over two cycles of its pitch, as its editor pictures it: what each
    // operator makes (at its level and envelope) and what is heard (the carriers' sum, a square root of their number down,
    // before the filter and the level), with each operator's envelope at the level given (1 for its peak) and the note at
    // full velocity, without the LFO, the pitch envelope or the spread, at 512 points a cycle. The waves are band-limited
    // and fed back as the synth plays them. Not for the audio thread (it computes a picture's worth at once).
    static constexpr int drawnPerCycle = 512, drawnLength = 2 * drawnPerCycle;

    struct Drawing
    {
        std::array<std::array<float, drawnLength>, numOperators> operators {};
        std::array<float, drawnLength> heard {};
    };

    static void draw (std::span<const float> parameters, std::span<const float, numOperators> envelopes, Drawing&);

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    double getTailSeconds() const override;

private:
    static constexpr int controlStep = 16; // samples between updates of pitch, levels, filter and LFO
    static constexpr int maxHeld = 16;     // keys mono mode remembers, to go back to
    static constexpr int maxLanes = 2;     // what a voice plays: one sound, or two spread apart

    // An operator's oscillator in one of a voice's lanes.
    struct Oscillator
    {
        float phase = 0.0f;                  // in cycles, from 0 to 1
        float increment = 0.0f;              // each sample at twice the rate
        float fedBack = 0.0f, before = 0.0f; // the last two of what its feedback takes, which averages them
        bool silent = false;                 // above what the rate can play
        Wavetable::Level saw, triangle;      // the band-limited waves' levels it reads at its pitch
    };

    // An operator's level in a voice, which both of its lanes play at.
    struct OperatorLevel
    {
        float noteGain = 1.0f; // what velocity and the key make of its level, set as its note starts
        float depth = 0.0f;    // its level now: a modulator's index in radians, a carrier's gain
        float gain = 0.0f;     // its level now as a gain, which what it feeds back is taken at
        float feedback = 0.0f; // its feedback's depth now in radians: above 0 its wave's, below 0 its wave squared's
    };

    // One of a voice's sounds: the only one, or the left or the right one while it is spread.
    struct Lane
    {
        std::array<Oscillator, numOperators> ops;
        dsp::Oversampler2x down;
        dsp::MultimodeFilter filter;
        float dcIn = 0.0f, dcOut = 0.0f; // what the DC blocker last took and gave
    };

    struct Voice
    {
        int note = -1;
        bool held = false, sustained = false;
        float velocity = 1.0f;
        float pitch = 60.0f, target = 60.0f; // in semitones, gliding to the target
        float slidePerSample = 0.0f;         // while it slides there evenly (a slide or portamento note)
        float pitchEnvelope = 0.0f;          // from 1 as its note starts, falling to 0
        float timeScale = 1.0f;              // its envelopes' times, against as set, by its key (keyTime)
        std::uint64_t started = 0;
        std::array<dsp::Adsr, numOperators> envelopes;
        std::array<OperatorLevel, numOperators> levels;
        std::array<Lane, maxLanes> lanes;
        int numLanes = 1; // two while its note is spread, set as the note starts
        dsp::FilterSettings filterSettings {};
        bool filterIsSet = false, active = false;
        float fade = 1.0f, fadeStep = 0.0f; // fading out: taken for another note, or cut as playback stopped
        int age = 0;
    };

    double sampleRate = 44100.0;
    static constexpr double stealFadeSeconds = 0.003;
    static constexpr double stopFadeSeconds = 0.01;
    static constexpr double justBegunSeconds = 0.1;
    static constexpr double portamentoSeconds = 0.1;
    static constexpr double dcBlockHertz = 5.0; // below it the DC blocker takes away what FM leaves off centre
    float dcBlock = 0.999f;                     // the DC blocker's pole at the sample rate
    bool songPlaying = false;

    // The last fades out what a voice played when another note took it (stealVoice()); notes never start on it.
    std::array<Voice, maxVoices + 1> voices;
    std::array<int, maxHeld> heldKeys {};
    int numHeld = 0;
    std::uint64_t notesPlayed = 0;
    float lastNote = -1.0f;
    float bend = 0.0f, modWheel = 0.0f;
    bool pedalDown = false;
    int shownMode = -1;
    const Wavetable* sawTable = nullptr;
    const Wavetable* triangleTable = nullptr;
    dsp::Lfo lfo;
    std::atomic<float> tailSeconds { 0.3f };
    std::atomic<int> activeVoices { 0 };
    Shown shown;

    void handle (const NoteEvent&, std::span<const float> parameters);
    void startNote (int note, float velocity, std::span<const float> parameters, bool portamento = false);
    void beginNote (Voice&, int note, float velocity, std::span<const float> parameters);
    static void setEnvelopes (Voice&, std::span<const float> parameters) noexcept;
    void slideTo (int note, float seconds);
    void stopNote (int note, std::span<const float> parameters);
    void releaseVoice (Voice&);
    Voice& findVoiceFor (int note);
    void stealVoice (Voice&);
    void render (float* left, float* right, int numSamples, std::span<const float> parameters);
};
} // namespace phazite::devices
