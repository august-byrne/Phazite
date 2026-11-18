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
// each note, each note's share of them coming in over a fade as a Juno's LFO delay does; and eight modulation slots, each a
// source moving a destination by an amount, for each voice on its own, so an envelope sweeps every note's table from its
// start; a slot may move another's amount, as the mod wheel brings an LFO's vibrato in, and Drift, a slow wander of each
// voice's own, lets its cutoff wander as an analog voice's does. 16 voices, or one in mono mode, legato, with glide (for
// every note, or only those played over a held one, as the synth's); in poly mode
// every note takes a voice of its own, a key played again too: what still rings of it rings out over its release,
// untouched, as the synth's does (Synth.h). It has no effects of its own: Phazite's devices follow it. Its noise, unison
// phases, drift and random values come from fixed seeds, so it renders the same every time.
#pragma once

#include "Wavetable.h"
#include "devices/core/DeviceInfo.h"
#include "devices/core/LiveValue.h"
#include "devices/core/Processor.h"
#include "devices/dsp/ControlGrid.h"
#include "devices/dsp/DcBlocker.h"
#include "devices/dsp/Envelopes.h"
#include "devices/dsp/MultimodeFilter.h"
#include "devices/dsp/Noise.h"
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
        lfo1FadeIn, // how long each note's share of the LFO takes to come in, from nothing
        lfo2Shape,
        lfo2Rate,
        lfo2Sync,
        lfo2Division,
        lfo2Retrigger,
        lfo2FadeIn,
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
        glideMode, // a GlideMode
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
        lfoFadeIn,
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
        sync,       // hard sync: the wave read 1 to 8 times a cycle (to 16 by modulation), starting again with each
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
        random, // a value for each note, from 0 to 1
        drift   // a slow wander of each voice's own, from -1 to 1, as the synth's Drift moves an analog voice's cutoff
    };

    // What a slot moves: a part of the sound, or another slot's amount (slot1Amount to slot8Amount: "Mod 1 Amount" to "Mod 8
    // Amount"), which the slots that aim at amounts move first, by their own amounts as set.
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
        unisonDetune,
        slot1Amount,
        slot2Amount,
        slot3Amount,
        slot4Amount,
        slot5Amount,
        slot6Amount,
        slot7Amount,
        slot8Amount
    };

    // The target that moves a slot's amount, from the first slot.
    static constexpr ModTarget getAmountTarget (int slot) noexcept { return (ModTarget) ((int) ModTarget::slot1Amount + slot); }

    enum class GlideMode
    {
        always, // every note glides from the one before
        legato  // only a note played while another is held, as the synth's (Synth::GlideMode)
    };

    static const DeviceInfo info;

    WavetableSynth(); // its oscillators on the first factory table
    ~WavetableSynth() override;

    // Message thread: what an oscillator plays, from the next control step on. A table the synth no longer plays is let go
    // by a later call, once the audio thread can't be reading it any more.
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
    void releaseUnused() override { freeRetired(); }

private:
    static constexpr int controlStep = 16;      // samples between updates of pitch, position, modulation and filter
    static constexpr int maxHeld = 16;          // keys mono mode remembers, to go back to
    static constexpr int numFading = maxVoices; // places to fade out voices taken for other notes: a chord's worth at once

    // An oscillator's unison voice: where it is in its cycle, and what it reads this control step.
    struct Reader
    {
        float phase = 0.0f, increment = 0.0f;
        float last = 0.0f;               // its wave's last sample, for the other oscillator's FM
        Wavetable::Level from {}, to {}; // the frames each side of its position, at the level its pitch allows
        float morph = 0.0f;              // from `from` toward `to`
        float offset = 0.0f;             // what its warp leaves off centre this control step, which it takes away
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

        // How many unison voices it plays, as its last control step found Unison Voices: the step sets as many readers, so a
        // count that changes between two steps (a block that ends between them, an event, automation) plays from the next.
        int unison = 1;
        std::array<OscillatorRun, numOscillators> oscillators;
        dsp::Oscillator sub;
        dsp::Noise noise;
        dsp::Random phases;
        dsp::Random drifting;                           // where its unison's drift wanders, the same every time
        dsp::Random wandering;                          // where the Drift source wanders, the same every time,
        Wander wander;                                  // and where it is (its pace the middle one)
        dsp::Adsr amp, envelope2, envelope3;            // the second and third at the control rate
        std::array<dsp::Lfo, 2> lfos;                   // those that start again with each note, at the control rate
        std::array<dsp::MultimodeFilter, 2> filters;    // each side
        std::array<dsp::AntialiasedShaper, 2> drives;   // the filter's drive, each side, at twice the rate
        float driveBlend = 0.0f;                        // how far the sound goes through them: fades as the drive comes on or off
        bool filtering = false;                         // the filter is on
        bool stepPending = true;                        // it (re)started since the last control step: its values are due
        int controlAhead = 0;                           // how far its control-rate parts are ahead of the samples it played (movesOn())
        float envelope2Now = 0.0f, envelope3Now = 0.0f; // their values as they last moved
        std::array<float, 2> lfosNow {};
        float fade = 1.0f, fadeStep = 0.0f; // fading out: taken for another note, or cut as playback stopped
        int age = 0;                        // the samples since its note began (up to a limit)
        float subGain = 0.0f, noiseGain = 0.0f, gainLeft = 1.0f, gainRight = 1.0f, driveGain = 1.0f;
        float cutoffNow = 0.0f; // its filter's frequency, as it was last set
    };

    struct Retired
    {
        std::shared_ptr<const Wavetable> table;
        std::uint64_t safeAfter = 0; // control steps begun from which the audio thread can't be reading it
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

    // Worked out from seconds as it is prepared, so that it keeps at any rate (Synth.h): how far the drive fades in a sample as
    // it comes on or off.
    float driveFadeStep = 1.0f / 32.0f;

    // The first maxVoices play notes; the rest fade out what voices played when other notes took them (stealVoice()), each
    // where it was copied, so that a chord that takes several at once fades each of them out.
    std::array<Voice, maxVoices + numFading> voices;
    std::array<int, maxHeld> heldKeys {};
    int numHeld = 0;
    std::uint64_t notesPlayed = 0;
    dsp::ControlGrid grid { controlStep };    // the control steps, on the song's own grid while it plays
    std::array<dsp::DcBlocker, 2> dcBlockers; // on each side of its sum: what ring modulation and FM leave off centre
    float lastNote = -1.0f;
    float bend = 0.0f, modWheel = 0.0f;
    bool pedalDown = false;
    bool stoppedPlaying = false; // playback stopped since the last note: once nothing sounds, the DC blockers' ring fades
    int shownMode = -1;
    std::array<dsp::Lfo, 2> freeLfos; // those that run on regardless of notes, at the control rate
    int freeLfosAhead = 0;            // how far they are ahead of the samples played (movesOn())
    std::array<float, 2> freeLfoValues {};
    dsp::Random noteRandom;

    // The tables the oscillators play: the message thread's own (owned), the one it gave last until the audio thread takes it
    // (pending), what the audio thread plays (stepTables), and what the message thread let go of until the audio thread
    // can't be reading it. The audio thread takes what is pending as a control step begins, after it counts the step, and
    // reads it until a later step takes another, which may be calls later: so a table it took is freed once the count has
    // moved on past where it was when the next was given (setTable()), and one it never took is freed as the next is given.
    // Sequentially consistent, so that the message thread's count after it gave a table can't be taken before it.
    std::array<std::atomic<const Wavetable*>, numOscillators> pending {};
    std::array<const Wavetable*, numOscillators> stepTables {}; // the audio thread's: what the control step under way reads
    std::array<std::shared_ptr<const Wavetable>, numOscillators> owned;
    std::vector<Retired> retired;
    std::atomic<std::uint64_t> stepsBegun { 0 };
    static_assert (std::atomic<std::uint64_t>::is_always_lock_free);

    std::array<std::atomic<float>, numOscillators> playedPositions;
    std::atomic<float> tailSeconds { 0.2f };
    std::atomic<int> activeVoices { 0 };
    Shown shown;

    void handle (const NoteEvent&, std::span<const float> parameters);
    void startNote (int note, float velocity, std::span<const float> parameters, bool portamento = false);
    void slideTo (int note, float seconds); // every sounding voice, evenly
    void stopNote (int note, std::span<const float> parameters);
    void startVoice (Voice&);
    void startDrift (Voice&) noexcept;               // a note's drift, each unison voice from somewhere new
    void moveDrift (Voice&, int unison) noexcept;    // a control step on
    void startWander (Voice&) noexcept;              // a note's Drift source, from somewhere new, as the synth's drift starts
    void moveWander (Voice&) noexcept;               // a control step on
    bool isAnotherKeyHeld (int note) const noexcept; // a voice but one playing this note is held: a legato note glides

    // Whether the control-rate parts move on at a step that starts now, each move a whole control step's, so that they keep
    // up with the samples played where a step runs short (a note starting within one, the grid cutting one short as a loop
    // goes round or the song jumps): they move unless they are already more than half a step ahead. `ahead` is how far
    // ahead of the samples played they are, which a move puts a step further and each stretch played brings back by its
    // length. A voice's parts move as it starts, whatever.
    static bool movesOn (int& ahead, bool whatever = false) noexcept;
    void releaseVoice (Voice&);
    void letGoOf (int note); // the voice its key or the pedal still holds, if any, lets go as its key coming up would
    Voice& findVoice();      // a voice for a new note: a free one, or one to take (stealVoice())
    void stealVoice (Voice&);
    void render (float* left, float* right, int numSamples, std::span<const float> parameters, bool playing, std::int64_t songSample);
    void freeRetired();
    static void renderStep (const OscillatorRun&, Reader&, float* out, int length) noexcept;
};
} // namespace phazite::devices
