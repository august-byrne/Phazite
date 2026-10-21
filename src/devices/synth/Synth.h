// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's synth (roadmap step 1.6): a subtractive synth with up to 16 voices. Each voice mixes two band-limited
// oscillators (the second tuned and detuned against the first), a sine an octave down and noise, through a resonant
// filter (one of the types of dsp/MultimodeFilter.h: low-pass, high-pass, band-pass or notch at 12 or 24 dB, comb, vowel,
// phaser, sample and hold) that has an envelope of its own and follows the keys, then an amp envelope. One LFO moves the
// pitch, the cutoff or the level; the mod wheel adds to it. In mono mode one voice plays, legato, gliding between the
// keys held, as a bassline does. Pitch bend and the sustain pedal work as on a keyboard. Unison plays each oscillator up
// to seven times at once, detuned against each other and spread across the stereo field, for a wide, thick sound (a
// "supersaw"). Its noise and its unison's starting phases come from fixed seeds, so it renders the same every time.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"
#include "devices/dsp/Envelopes.h"
#include "devices/dsp/MultimodeFilter.h"
#include "devices/dsp/Oscillators.h"
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
        oscMix,
        pulseWidth,
        subLevel,
        noiseLevel,
        unisonVoices, // from one voice up to seven, detuned and spread
        unisonDetune,
        unisonSpread,
        filterType, // a dsp::FilterMode
        cutoff,
        resonance,
        filterSlope,
        filterEnvelope,
        keyTracking,
        filterAttack,
        filterDecay,
        filterSustain,
        filterRelease,
        attack,
        decay,
        sustain,
        release,
        lfoShape,
        lfoTarget,
        lfoRate,
        lfoDepth,
        voiceMode,
        glide,
        bendRange,
        velocity,
        level
    };

    enum class LfoTarget
    {
        pitch,  // up to a semitone either way
        cutoff, // up to two octaves either way
        level   // tremolo
    };

    static const DeviceInfo info;

    // How many voices are sounding, for tests and meters. Any thread.
    int getActiveVoices() const noexcept { return activeVoices.load (std::memory_order_relaxed); }

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
        float pitch = 60.0f, target = 60.0f;               // in semitones, gliding to the target
        std::uint64_t started = 0;                         // when it began, so that the oldest is taken first
        std::array<dsp::Oscillator, maxUnison> osc1, osc2; // one of each per unison voice
        dsp::Oscillator sub;
        dsp::Random unisonPhases; // where the unison's oscillators start, the same every time
        dsp::Random noise;
        dsp::Adsr amp, filterEnvelope;
        std::array<dsp::MultimodeFilter, 2> filters; // the second only while unison spreads the voice across the stereo field
        dsp::FilterSettings filterSettings {};       // what they were last set to,
        bool filterIsSet = false;                    // if they were since the voice started
        float filterEnvelopeLevel = 0.0f;
        float fade = 1.0f, fadeStep = 0.0f; // fading out: taken for another note, or cut as playback stopped
        int age = 0;                        // the samples since its note began (up to a limit)
    };

    double sampleRate = 44100.0;
    static constexpr double stealFadeSeconds = 0.003; // a voice taken for another note fades out over this
    static constexpr double stopFadeSeconds = 0.01;   // a note that had only just begun when playback stopped, over this
    static constexpr double justBegunSeconds = 0.1;   // how young that is
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

    void handle (const NoteEvent&, std::span<const float> parameters);
    void startNote (int note, float velocity, std::span<const float> parameters);
    void stopNote (int note, std::span<const float> parameters);
    void releaseVoice (Voice&);
    Voice& findVoiceFor (int note);
    void stealVoice (Voice&);
    void render (float* left, float* right, int numSamples, std::span<const float> parameters);
};
} // namespace phazite::devices
