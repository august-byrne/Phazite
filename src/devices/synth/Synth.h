// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's synth (roadmap step 1.6): a subtractive synth with up to 16 voices. Each voice mixes two band-limited
// oscillators (the second tuned and detuned against the first, and hard-synced to it if wanted, its pitch swept by the
// filter envelope, as a Prophet-5's poly-mod sweeps its synced oscillator), a sub an octave or two down (a sine, triangle,
// saw or square) and noise, through a resonant filter (one of the types of dsp/MultimodeFilter.h: low-pass, high-pass,
// band-pass or notch at 12 or 24 dB, comb, vowel, phaser, sample and hold) that has an envelope of its own, follows the keys
// and closes for softer notes as much as wanted, then an amp envelope. A pitch envelope drops each note onto its pitch,
// for 808s, toms and zaps. One LFO moves the pitch, the cutoff, the level or the pulse width (a Juno's string pad), coming
// in over a fade after each note starts as a Juno's LFO delay does; the mod wheel adds to it. Drift lets each voice's
// oscillators and cutoff wander a little, as an analog synth's do. In mono mode one voice plays, legato, gliding between
// the keys held, as a bassline does, every note or only those played over a held one (a TB-303's slides). Pitch bend and
// the sustain pedal work as on a keyboard. Unison plays each oscillator up to seven times at once, detuned against each
// other and spread across the stereo field, for a wide, thick sound (a "supersaw"). Its noise, its unison's starting
// phases and its drift come from fixed seeds, so it renders the same every time.
#pragma once

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

namespace phazite::devices
{
class Synth final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteSynth";
    static constexpr int maxVoices = 16;
    static constexpr int maxUnison = 7; // oscillators each voice plays at once, detuned against each other

    // In the order of the description's parameters, as the editor shows them.
    enum Parameter
    {
        osc1Wave,
        osc2Wave,
        osc2Pitch,
        osc2Detune,
        osc2Sync,     // osc 2 starts its cycle again with each of osc 1's (hard sync)
        osc2Envelope, // how far the filter envelope moves osc 2's pitch, in semitones: with Sync, the sync sweep
        oscMix,
        pulseWidth,
        subLevel,
        subOctave, // one octave down, or two
        subShape,  // a dsp::Waveform: sine, triangle, saw or square
        noiseLevel,
        drift,        // how far each voice's oscillators and cutoff wander
        unisonVoices, // from one voice up to seven, detuned and spread
        unisonDetune,
        unisonSpread,
        filterType, // a dsp::FilterMode
        cutoff,
        resonance,
        filterSlope,
        drive, // the filter's, saturating what goes into it, in decibels
        filterEnvelope,
        keyTracking,
        filterVelocity, // how far a softer note's cutoff falls: at full, four octaves at velocity 0
        filterAttack,
        filterDecay,
        filterSustain,
        filterRelease,
        filterAttackCurve, // how each stage bends (dsp::Adsr::setCurves())
        filterDecayCurve,
        filterReleaseCurve,
        attack,
        decay,
        sustain,
        release,
        attackCurve,
        decayCurve,
        releaseCurve,
        lfoShape,
        lfoTarget,
        lfoRate,
        lfoDepth,
        lfoFade,     // how long each note's LFO takes to come in, from nothing to its depth
        pitchAmount, // the pitch envelope: semitones each note starts above (or below) its pitch,
        pitchDecay,  // falling onto it over this time
        voiceMode,
        glide,
        glideMode, // a GlideMode
        bendRange,
        velocity,
        level,
        filterOn, // the parts that switch on and off: the filter and its envelope, at first on; the LFO and the unison, off
        filterEnvelopeOn,
        lfoOn,
        unisonOn
    };

    enum class LfoTarget
    {
        pitch,     // up to a semitone either way
        cutoff,    // up to two octaves either way
        level,     // tremolo
        pulseWidth // up to 45 % either way, as a Juno's PWM: a square's width, and so its colour, moving
    };

    enum class GlideMode
    {
        always, // every note glides from the one before
        legato  // only a note played while another is held, as a TB-303 slides
    };

    static const DeviceInfo info;

    // How many voices are sounding, for tests and meters. Any thread.
    int getActiveVoices() const noexcept { return activeVoices.load (std::memory_order_relaxed); }

    // What its editor shows playing, once a block (devices/core/LiveValue.h): the newest voice's amp and filter envelopes
    // (from 0 to 1) and the cutoff it plays (in hertz; 0 while no voice plays), and where the LFO is in its cycle (0 to 1).
    struct Shown
    {
        LiveValue amp, filterEnvelope, cutoff, lfoPhase;
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
    static constexpr int controlStep = 16; // samples between updates of pitch, filter and LFO
    static constexpr int maxHeld = 16;     // keys mono mode remembers, to go back to

    struct Voice
    {
        int note = -1;
        bool held = false;      // its key is down
        bool sustained = false; // its key is up but the sustain pedal holds it
        float velocityGain = 1.0f;
        float velocity = 1.0f;                             // its note's, from 0 to 1, which the filter's velocity takes
        float pitchEnvelope = 0.0f;                        // the pitch envelope, from 1 as the note starts toward 0
        float pitch = 60.0f, target = 60.0f;               // in semitones, gliding to the target
        float slidePerSample = 0.0f;                       // while it slides there evenly (a slide or portamento note): semitones a sample
        std::uint64_t started = 0;                         // when it began, so that the oldest is taken first
        std::array<dsp::Oscillator, maxUnison> osc1, osc2; // one of each per unison voice
        dsp::Oscillator sub;
        dsp::Random unisonPhases; // where the unison's oscillators start, the same every time
        dsp::Random noise;
        dsp::Random drifting;                         // where its drift wanders, the same every time
        std::array<float, 3> driftNow {}, driftTo {}; // osc 1's, osc 2's and the cutoff's, from -1 to 1
        int driftStepsLeft = 0;                       // control steps until it wanders toward new places
        dsp::Adsr amp, filterEnvelope;
        std::array<dsp::MultimodeFilter, 2> filters;  // the second only while unison spreads the voice across the stereo field
        std::array<dsp::AntialiasedShaper, 2> drives; // the filter's drive, each side, at twice the rate
        float driveBlend = 0.0f;                      // how far the sound goes through them: fades as the drive comes on or off
        dsp::FilterSettings filterSettings {};        // what they were last set to,
        bool filterIsSet = false;                     // if they were since the voice started
        float filterEnvelopeLevel = 0.0f;
        float fade = 1.0f, fadeStep = 0.0f; // fading out: taken for another note, or cut as playback stopped
        int age = 0;                        // the samples since its note began (up to a limit)
    };

    double sampleRate = 44100.0;
    static constexpr double stealFadeSeconds = 0.003; // a voice taken for another note fades out over this
    static constexpr double stopFadeSeconds = 0.01;   // a note that had only just begun when playback stopped, over this
    static constexpr double justBegunSeconds = 0.1;   // how young that is
    static constexpr double portamentoSeconds = 0.1;  // a portamento note's glide while Glide is off
    bool songPlaying = false;                         // as the block being processed began

    // The last fades out what a voice played when another note took it (stealVoice()); notes never start on it.
    std::array<Voice, maxVoices + 1> voices;
    std::array<int, maxHeld> heldKeys {}; // mono mode's keys down, oldest first
    int numHeld = 0;
    std::uint64_t notesPlayed = 0;
    float lastNote = -1.0f; // where a new note glides from
    float bend = 0.0f, modWheel = 0.0f;
    bool pedalDown = false;
    int shownMode = -1; // poly (0) or mono (1) as the last block played
    dsp::Lfo lfo;
    std::atomic<float> tailSeconds { 0.2f };
    std::atomic<int> activeVoices { 0 };
    Shown shown;

    void handle (const NoteEvent&, std::span<const float> parameters);
    void startNote (int note, float velocity, std::span<const float> parameters, bool portamento = false);
    void slideTo (int note, float seconds); // every sounding voice, evenly
    void stopNote (int note, std::span<const float> parameters);
    void releaseVoice (Voice&);
    Voice& findVoiceFor (int note);
    void stealVoice (Voice&);
    bool isAnotherKeyHeld (int note) const noexcept; // a voice but one playing this note is held: a legato note glides
    void startDrift (Voice&) noexcept;               // a note's drift, from somewhere new
    void moveDrift (Voice&) noexcept;                // a control step on
    void render (float* left, float* right, int numSamples, std::span<const float> parameters);
};
} // namespace phazite::devices
