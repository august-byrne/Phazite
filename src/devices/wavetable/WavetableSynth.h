// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's wavetable synth (roadmap step 2.13), "Wavetable": Serum's layout with Ableton Live's Wavetable's simplicity
// (docs/design.md, section 9). Two wavetable oscillators morph through their tables' frames (Wavetable.h: a factory table
// or a sound of the user's), each with a pitch, a level and a warp that reshapes its wave (hard sync, bend, pulse width,
// mirror, or FM or ring modulation from the other oscillator); then a sub oscillator and noise. Unison plays both
// oscillators up to eight times at once, detuned, spread across the stereo field and across the table; its stack puts
// every second or third of those copies an octave or two up, or a fifth, as Serum's unison stack and Vital's stack styles
// do (an octave pile or a power chord from one key), and its drift lets each copy's pitch wander on its own, as
// ZynAddSubFX's unison vibrato does, the way the synth's Drift lets an analog voice's oscillators wander. A resonant filter
// with drive follows, of any type of dsp/MultimodeFilter.h (low-pass, high-pass, band-pass or notch at 12 or 24 dB, comb,
// vowel, phaser, sample and hold); three envelopes (the first shapes the level) and two LFOs, free or starting again with
// each note; and eight modulation slots, each a source moving a destination by an amount, for each voice on its own, so
// an envelope sweeps every note's table from its start. 16 voices, or one in mono mode, legato, with glide; in poly mode
// every note takes a voice of its own, a key played again too: what still rings of it rings out over its release,
// untouched, as the synth's does (Synth.h). It has no effects of its own: Phazite's devices follow it. Its noise, unison
// phases, drift and random values come from fixed seeds, so it renders the same every time.
#pragma once

#include "Wavetable.h"
#include "devices/core/DeviceInfo.h"
#include "devices/core/LiveValue.h"
#include "devices/core/Processor.h"
#include "devices/dsp/Envelopes.h"
#include "devices/dsp/MultimodeFilter.h"
#include "devices/dsp/Oscillators.h"
#include "devices/dsp/Oversampling.h"
#include "devices/dsp/Random.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace phazite::devices
{
class WavetableSynth final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteWavetable";
    static constexpr int numOscillators = 2, maxVoices = 16, maxUnison = 8, numModSlots = 8;

    // In the order of the description's parameters, whose IDs are saved in projects.
    enum Parameter
    {
        osc1On,
        osc1Position,
        osc1Pitch,
        osc1Fine,
        osc1Level,
        osc1Warp,
        osc1WarpAmount,
        osc2On,
        osc2Position,
        osc2Pitch,
        osc2Fine,
        osc2Level,
        osc2Warp,
        osc2WarpAmount,
        subLevel,
        subOctave,
        subShape,
        noiseLevel,
        unisonVoices,
        unisonDetune,
        unisonSpread,
        unisonPosition,
        unisonStack, // a Stack
        unisonDrift, // how far each copy's pitch wanders, in cents
        filterOn,
        filterType, // a dsp::FilterMode
        cutoff,
        resonance,
        filterSlope,
        drive,
        keyTracking,
        env1Attack,
        env1Decay,
        env1Sustain,
        env1Release,
        env1AttackCurve, // how each stage bends (dsp::Adsr::setCurves())
        env1DecayCurve,
        env1ReleaseCurve,
        env2Attack,
        env2Decay,
        env2Sustain,
        env2Release,
        env2AttackCurve, // how each stage bends (dsp::Adsr::setCurves())
        env2DecayCurve,
        env2ReleaseCurve,
        env3Attack,
        env3Decay,
        env3Sustain,
        env3Release,
        env3AttackCurve, // how each stage bends (dsp::Adsr::setCurves())
        env3DecayCurve,
        env3ReleaseCurve,
        lfo1Shape,
        lfo1Rate,
        lfo1Sync,
        lfo1Division,
        lfo1Retrigger,
        lfo2Shape,
        lfo2Rate,
        lfo2Sync,
        lfo2Division,
        lfo2Retrigger,
        mod1Source,
        mod1Target,
        mod1Amount,
        mod2Source,
        mod2Target,
        mod2Amount,
        mod3Source,
        mod3Target,
        mod3Amount,
        mod4Source,
        mod4Target,
        mod4Amount,
        mod5Source,
        mod5Target,
        mod5Amount,
        mod6Source,
        mod6Target,
        mod6Amount,
        mod7Source,
        mod7Target,
        mod7Amount,
        mod8Source,
        mod8Target,
        mod8Amount,
        voiceMode,
        glide,
        bendRange,
        velocity,
        level
    };

    // An oscillator's parameters, from osc1On or osc2On; an envelope's from env1Attack; an LFO's from lfo1Shape; a
    // modulation slot's from mod1Source.
    enum OscillatorField
    {
        oscOn,
        oscPosition,
        oscPitch,
        oscFine,
        oscLevel,
        oscWarp,
        oscWarpAmount,
        oscFields
    };

    enum EnvelopeField
    {
        envAttack,
        envDecay,
        envSustain,
        envRelease,
        envAttackCurve,
        envDecayCurve,
        envReleaseCurve,
        envFields
    };

    enum LfoField
    {
        lfoShape,
        lfoRate,
        lfoSync,
        lfoDivision,
        lfoRetrigger,
        lfoFields
    };

    enum ModField
    {
        modSource,
        modTarget,
        modAmount,
        modFields
    };

    static constexpr int getParameter (int osc, OscillatorField field) noexcept { return osc1On + osc * oscFields + field; }
    static constexpr int getEnvelopeParameter (int envelope, EnvelopeField field) noexcept
    {
        return env1Attack + envelope * envFields + field;
    }

    static constexpr int getLfoParameter (int lfo, LfoField field) noexcept { return lfo1Shape + lfo * lfoFields + field; }
    static constexpr int getModParameter (int slot, ModField field) noexcept { return mod1Source + slot * modFields + field; }

    enum class Warp
    {
        none,
        sync,       // hard sync: the wave read 1 to 8 times a cycle, starting again with each
        bendUp,     // the wave's start squeezed, its end stretched
        bendDown,   // the other way
        pulseWidth, // the whole wave in the first part of the cycle, then its start held
        mirror,     // the wave read forward, then back
        fm,         // the other oscillator moves its phase
        ringMod     // the other oscillator multiplies it
    };

    // Where the unison's copies play, one after another from the first: all on the note, or every second or third of them
    // above it, an octave or two up, or a fifth and an octave, as Serum's unison stack and Vital's stack styles put theirs.
    // With two copies or more.
    enum class Stack
    {
        off,
        octave,     // the note, an octave up, the note, ...
        twoOctaves, // the note, an octave up, two octaves up, the note, ...
        fifth,      // the note, a fifth up, ...
        powerChord  // the note, a fifth up, an octave up, ...
    };

    static constexpr int maxStackSteps = 3;

    // A stack's steps above the note, in semitones, which the copies take in turn.
    static std::span<const float> getStackSteps (Stack) noexcept;

    enum class ModSource
    {
        none,
        envelope2,
        envelope3,
        lfo1,
        lfo2,
        velocity,
        note, // the key, centred on middle C (60), an octave each side for each 0.25
        modWheel,
        random // a value for each note, from 0 to 1
    };

    enum class ModTarget
    {
        none,
        osc1Position,
        osc1Warp,
        osc1Pitch,
        osc1Level,
        osc2Position,
        osc2Warp,
        osc2Pitch,
        osc2Level,
        pitch, // both oscillators and the sub
        subLevel,
        noiseLevel,
        cutoff,
        resonance,
        volume,
        pan,
        unisonDetune
    };

    static const DeviceInfo info;

    WavetableSynth(); // its oscillators on the first factory table
    ~WavetableSynth() override;

    // Message thread: what an oscillator plays. A table the synth no longer plays is let go by a later call, once the
    // audio thread can't be playing it any more.
    void setTable (int osc, std::shared_ptr<const Wavetable>);
    std::shared_ptr<const Wavetable> getTable (int osc) const;

    // Where in its table each oscillator plays the newest note, its position moved by its modulation, from 0 to 1: for
    // the editor to show. Any thread.
    float getPlayedPosition (int osc) const noexcept;

    // How many voices are sounding, for tests and meters. Any thread.
    int getActiveVoices() const noexcept { return activeVoices.load (std::memory_order_relaxed); }

    // What its editor shows playing, once a block (devices/core/LiveValue.h): the newest note's three envelopes (from 0 to 1)
    // and the cutoff it plays (in hertz; 0 while no note plays or the filter is off), and where each LFO is in its cycle (0 to
    // 1; below 0 for one that starts with each note while none plays).
    struct Shown
    {
        std::array<LiveValue, 3> envelopes;
        LiveValue cutoff;
        std::array<LiveValue, 2> lfoPhases;
    };

    const Shown& getShown() const noexcept { return shown; }

    // The filter's response at a frequency, as a gain, for the parameters' values with its cutoff at a frequency: for its
    // editor.
    static double getFilterMagnitude (std::span<const float> parameters, double cutoffHertz, double frequency, double sampleRate);

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    double getTailSeconds() const override;

private:
    static constexpr int controlStep = 16; // samples between updates of pitch, position, modulation and filter
    static constexpr int maxHeld = 16;     // keys mono mode remembers, to go back to

    // An oscillator's unison voice: where it is in its cycle, and what it reads this control step.
    struct Reader
    {
        float phase = 0.0f, increment = 0.0f;
        float last = 0.0f;               // its wave's last sample, for the other oscillator's FM
        Wavetable::Level from {}, to {}; // the frames each side of its position, at the level its pitch allows
        float morph = 0.0f;              // from `from` toward `to`
        float left = 1.0f, right = 1.0f;
    };

    // Where a unison voice's pitch has wandered (the unison's drift), from -1 to 1, toward a new place every so often, at
    // a pace of its own.
    struct Wander
    {
        float now = 0.0f, to = 0.0f;
        float pace = 1.0f; // how fast it wanders against the others, from half to twice
        int stepsLeft = 0; // control steps until it wanders toward a new place
    };

    struct OscillatorRun
    {
        bool on = false;
        Warp warp = Warp::none;
        float warpAmount = 0.0f, level = 0.0f, position = 0.0f;
        float syncRatio = 1.0f, bend = 1.0f, pulseWidth = 1.0f; // what its warp reads by, this control step
        std::array<Reader, maxUnison> readers {};
        std::array<Wander, maxUnison> wanders {};
    };

    struct Voice
    {
        int note = -1;
        bool held = false, sustained = false;
        float velocity = 0.0f, velocityGain = 1.0f, random = 0.0f;
        float pitch = 60.0f, target = 60.0f; // in semitones, gliding to the target
        float slidePerSample = 0.0f;         // while it slides there evenly (a slide or portamento note): semitones a sample
        std::uint64_t started = 0;
        std::array<OscillatorRun, numOscillators> oscillators;
        dsp::Oscillator sub;
        dsp::Random noise, phases;
        dsp::Random drifting;                         // where its unison's drift wanders, the same every time
        dsp::Adsr amp, envelope2, envelope3;          // the second and third at the control rate
        std::array<dsp::Lfo, 2> lfos;                 // those that start again with each note, at the control rate
        std::array<dsp::MultimodeFilter, 2> filters;  // each side
        std::array<dsp::AntialiasedShaper, 2> drives; // the filter's drive, each side, at twice the rate
        float driveBlend = 0.0f;                      // how far the sound goes through them: fades as the drive comes on or off
        bool filtering = false;                       // the filter is on
        bool stepPending = true;                      // it (re)started since the last control step: its values are due
        float fade = 1.0f, fadeStep = 0.0f;           // fading out: taken for another note, or cut as playback stopped
        int age = 0;                                  // the samples since its note began (up to a limit)
        float subGain = 0.0f, noiseGain = 0.0f, gainLeft = 1.0f, gainRight = 1.0f, driveGain = 1.0f;
        float cutoffNow = 0.0f; // its filter's frequency, as it was last set
    };

    struct Retired
    {
        std::shared_ptr<const Wavetable> table;
        std::uint64_t safeAfter = 0; // blocks done after which the audio thread can't be reading it
    };

    double sampleRate = 44100.0;
    double aliasLimit = 0.5; // the highest frequency, as a fraction of the sample rate, whose alias stays above hearing
    static constexpr double stealFadeSeconds = 0.003; // a voice taken for another note fades out over this
    static constexpr double stopFadeSeconds = 0.01;   // a note that had only just begun when playback stopped, over this
    static constexpr double justBegunSeconds = 0.1;   // how young that is
    static constexpr double portamentoSeconds = 0.1;  // a portamento note's glide while Glide is off
    bool songPlaying = false;                         // as the block being processed began

    // The unison's drift, at the control rate: the steps between a unison voice's new places, and how far it glides toward
    // its place each step, for one at the middle pace.
    float driftWanderSteps = 1.0f, driftFollow = 1.0f;

    // The last fades out what a voice played when another note took it (stealVoice()); notes never start on it.
    std::array<Voice, maxVoices + 1> voices;
    std::array<int, maxHeld> heldKeys {};
    int numHeld = 0;
    std::uint64_t notesPlayed = 0;
    int samplesLeftInStep = 0; // of the control step under way, which goes on across blocks and the events that split them
    float lastNote = -1.0f;
    float bend = 0.0f, modWheel = 0.0f;
    bool pedalDown = false;
    int shownMode = -1;
    std::array<dsp::Lfo, 2> freeLfos; // those that run on regardless of notes, at the control rate
    std::array<float, 2> freeLfoValues {};
    dsp::Random noteRandom;

    std::array<std::atomic<const Wavetable*>, numOscillators> tables;
    std::array<std::shared_ptr<const Wavetable>, numOscillators> owned; // the message thread's
    std::vector<Retired> retired;
    std::atomic<std::uint64_t> blocksDone { 0 };

    std::array<std::atomic<float>, numOscillators> playedPositions;
    std::atomic<float> tailSeconds { 0.2f };
    std::atomic<int> activeVoices { 0 };
    Shown shown;

    void handle (const NoteEvent&, std::span<const float> parameters);
    void startNote (int note, float velocity, std::span<const float> parameters, bool portamento = false);
    void slideTo (int note, float seconds); // every sounding voice, evenly
    void stopNote (int note, std::span<const float> parameters);
    void startVoice (Voice&);
    void startDrift (Voice&) noexcept;            // a note's drift, each unison voice from somewhere new
    void moveDrift (Voice&, int unison) noexcept; // a control step on
    void releaseVoice (Voice&);
    void letGoOf (int note); // the voice its key or the pedal still holds, if any, lets go as its key coming up would
    Voice& findVoice();      // a voice for a new note: a free one, or one to take (stealVoice())
    void stealVoice (Voice&);
    void render (float* left, float* right, int numSamples, std::span<const float> parameters);
    void freeRetired();
    static void renderStep (const OscillatorRun&, Reader&, float* out, int length) noexcept;
};
} // namespace phazite::devices
