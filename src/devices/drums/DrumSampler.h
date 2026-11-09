// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's drum sampler (roadmap steps 1.6 and 4.12): a pad on every MIDI note, as Ableton's Drum Rack has, its editor
// showing sixteen at a time; General MIDI's drums from note 36, where the default kit sits, as FL Studio's FPC and an MPC
// lay them out. Each pad plays a sound from where its start says, forwards or reversed, at its level, pan and tuning
// (semitones and cents), through an envelope (an attack that softens its start, a hold at full level, then a decay to
// silence), as a one-shot that plays on however short the note, or gated, its note's end starting its decay; swept from
// above its pitch (or below) back to it as a synthesized drum is, through a filter (low-pass, high-pass or band-pass), and
// with velocity, which turns it down and closes its filter by as much as it says. Its level, pan and filter follow their
// values as it rings (automation, a knob turned); the rest is set as it is hit. A pad can have up to four sounds, its
// layers, each at a level of its own, which share its hits as its layer mode says: stacked, by velocity, in turn or at
// random. Pads in the same choke group stop each other, as an open hat stops when the closed one plays; a pad muted
// plays nothing, and while any pad is soloed only the soloed pads play. A new sampler plays the default kit of the sounds
// Phazite synthesizes (FactoryDrums.h); a pad can play another of them, from the kits that come with Phazite, or a sound
// file, which the model loads (model/devices/internal/DrumKit.h), and which the sampler swaps in without the audio
// thread ever waiting or freeing memory.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/LiveValue.h"
#include "devices/core/MeterValue.h"
#include "devices/core/Processor.h"
#include "devices/dsp/Random.h"
#include "devices/dsp/SincReader.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace phazite::devices
{
class DrumSampler final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteDrums";

    // A pad on every note, the pad its note's number, with up to numLayers sounds; the default kit on the sixteen from
    // kitFirstNote, a kick to a ride.
    static constexpr int numPads = 128, firstNote = 0, maxVoices = 32, numLayers = 4;
    static constexpr int kitFirstNote = 36, kitPads = 16;

    // A pad's parameters, in this order: pad p's are from p * fieldsPerPad on. The output's level is last.
    enum Field
    {
        level,
        pan,
        tune,      // semitones
        fine,      // cents
        start,     // where in its sound it starts, from 0 to 1 of its length (from its end while reversed)
        reverse,   // played backwards
        attack,    // ms: in from silence
        hold,      // ms at full level after the attack; at its longest, the whole sound
        decay,     // ms from full level to silence, after the hold (or, gated, after the note's end)
        mode,      // One Shot or Gate
        sweep,     // semitones above (or below) its pitch that it starts at, sweeping back to it as a synthesized drum does
        sweepTime, // ms: how fast it sweeps back, the sweep's time constant
        filter,    // Off, Low Pass, High Pass or Band Pass
        cutoff,
        resonance,
        velocity,         // how much a softer hit turns it down, from 0 (not at all) to 1 (as the velocity says)
        velocityToCutoff, // semitones a hit at velocity 0 closes its filter by, a harder one less
        choke,            // its choke group: off, or 1 to 8
        mute,
        solo,
        layerMode,   // how its sounds share its hits (LayerMode)
        layer1Level, // each sound's level against the others', in decibels
        layer2Level,
        layer3Level,
        layer4Level
    };

    static constexpr int fieldsPerPad = 25;
    static constexpr int outputParameter = numPads * fieldsPerPad;
    static constexpr int getParameterIndex (int pad, Field field) noexcept { return pad * fieldsPerPad + (int) field; }

    enum class Mode
    {
        oneShot,
        gate
    };

    enum class FilterType
    {
        off,
        lowPass,
        highPass,
        bandPass
    };

    // How a pad's sounds share its hits: all of them at once; one by velocity, the softest hits the first's, the range split
    // evenly between them; each in turn; or one at random, never the same twice in a row.
    enum class LayerMode
    {
        stack,
        velocity,
        roundRobin,
        random
    };

    static constexpr float wholeSound = 10000.0f; // ms: a hold this long plays the whole sound

    static const DeviceInfo info;

    // A sound a pad plays, at its own sample rate, in mono or stereo.
    struct Sound
    {
        juce::AudioBuffer<float> audio;
        double sampleRate = 48000.0;
    };

    DrumSampler();
    ~DrumSampler() override;

    // Message thread. What a pad's layer plays: a sound given it (a factory sound, or a file's), or with nullptr its default:
    // the first layer's on the default kit's sixteen pads is the kit's, and nothing otherwise. A sound the sampler no longer
    // plays is let go by a later call, once the audio thread can't be playing it any more.
    void setSound (int pad, int layer, std::shared_ptr<const Sound>);
    void setSound (int pad, std::shared_ptr<const Sound> sound) { setSound (pad, 0, std::move (sound)); }
    bool hasOwnSound (int pad, int layer = 0) const noexcept; // given one, rather than playing its default

    // The default kit's sound a pad plays when given none: its factory sound, or none off the kit's sixteen pads.
    static std::shared_ptr<const Sound> getDefaultSound (int pad);

    // Plays a pad at the next block, as if its note came in: for the editor's pads. Message thread, lock-free. Played while
    // the sampler plays no blocks (switched off, or on a muted or frozen track), it never plays: one that waited longer
    // than longestAuditionWait is dropped, rather than heard late with every pad played since.
    void audition (int pad, float velocity);
    bool isWaitingToPlay() const noexcept override; // a pad played, for the next block

    // How hard each pad was hit since the editor last looked, from 0 to 1 (devices/core/MeterValue.h).
    MeterValue& getPadMeter (int pad) noexcept { return padMeters[(size_t) pad]; }

    // Message thread: the sound a pad's layer plays now, for drawing it; none for a layer without one.
    const Sound* getPadSound (int pad, int layer = 0) const noexcept { return playing[(size_t) pad][(size_t) layer].get(); }

    // How far each pad's newest hit is through its sound, from 0 to 1 (from its start, or its end while reversed), or below 0
    // while it plays none (devices/core/LiveValue.h).
    const LiveValue& getPlayhead (int pad) const noexcept { return playheads[(size_t) pad]; }

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    double getTailSeconds() const override;

private:
    struct Voice
    {
        int pad = -1, layer = 0; // none: the voice is free
        const Sound* sound = nullptr;
        float velocity = 1.0f;
        double position = 0.0, step = 1.0; // where it reads, forwards from its start (from its end while reversed)
        bool reversed = false;
        float gainLeft = 1.0f, gainRight = 1.0f;
        int played = 0, attackSamples = 0, holdSamples = 0;
        float decayStep = 1.0f; // what its level is multiplied by each sample while it decays
        float level = 1.0f;     // its envelope after the attack: 1 while it holds, falling once it decays
        bool decaying = false, released = false;
        float fade = 1.0f, fadeStep = 0.0f;        // a cut's or a choke's fade, once fadeStep is above 0
        double pitchOffset = 0.0, pitchFall = 1.0; // semitones it is bent by, and what that is multiplied by each sample

        // Its filter, a state-variable filter (Cytomic's), and each side's state.
        FilterType filterType = FilterType::off;
        float g = 0.0f, k = 2.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
        std::array<float, 2> ic1 {}, ic2 {};
        std::uint64_t started = 0;
    };

    struct Retired
    {
        std::shared_ptr<const Sound> sound;
        std::uint64_t safeAfter = 0; // blocks done after which the audio thread can't be playing it
    };

    template <typename Each>
    using PerLayer = std::array<std::array<Each, numLayers>, numPads>;

    double sampleRate = 44100.0;
    PerLayer<std::atomic<const Sound*>> sounds;
    std::array<Voice, maxVoices> voices {};
    dsp::SincReader reader; // for a pad tuned, or at another rate than the engine's
    std::uint64_t hits = 0;
    std::atomic<std::uint64_t> blocksDone { 0 };

    // The audio thread's: each pad's next sound in turn, and the one it played last at random; and what picks one.
    std::array<std::uint8_t, numPads> nextLayer {}, lastLayer {};
    dsp::Random random;

    // The message thread's: what each pad's layers play, whether they were given that, and the sounds let go of, held until
    // the audio thread can't be playing them.
    PerLayer<std::shared_ptr<const Sound>> playing;
    PerLayer<bool> given {};
    std::vector<Retired> retired;

    // The pads the editor played, each with when (juce::Time::getMillisecondCounter()).
    struct Audition
    {
        int pad = -1;
        float velocity = 0.0f;
        juce::uint32 playedAt = 0;
    };

    static constexpr int auditionSlots = 64;
    static constexpr int longestAuditionWait = 250; // ms: longer than an audio device's longest blocks last
    juce::AbstractFifo auditionFifo { auditionSlots };
    std::array<Audition, auditionSlots> auditions {};

    std::array<MeterValue, numPads> padMeters;
    std::array<LiveValue, numPads> playheads;

    void hit (int pad, float velocity, std::span<const float> parameters);
    void startVoice (int pad, int layer, const Sound&, float velocity, std::span<const float> parameters); // a layer's
    void release (int pad);
    void applyValues (Voice&, std::span<const float> parameters); // its level, pan and filter, as its pad's values are now
    void follow (std::span<const float> parameters);              // every voice's, and fades those of pads that mute or solo leave out
    void render (float* left, float* right, int numSamples, std::span<const PartOutput>, int outputStart);
    void freeRetired();
};
} // namespace phazite::devices
