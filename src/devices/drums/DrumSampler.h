// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's drum sampler (roadmap step 1.6): 16 pads on the notes from 36 up, General MIDI's drums, as FL Studio's FPC
// and Ableton's Drum Rack lay them out. Each pad plays a sound once, whole, however short the note (a one-shot), at its
// level, pan and tuning, with an attack that softens its start and a length that cuts it short. Pads in the same choke
// group stop each other, as an open hat stops when the closed one plays. A new sampler plays a kit that Phazite
// synthesizes (DrumSynthesis.h); a pad can play a sound file instead, which its thin plug-in loads, and which the
// sampler swaps in without the audio thread ever waiting or freeing memory.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/MeterValue.h"
#include "devices/core/Processor.h"

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
    static constexpr int numPads = 16, firstNote = 36, maxVoices = 32;

    // A pad's parameters, in this order: pad p's are from p * fieldsPerPad on. The output's level is last.
    enum Field
    {
        level,
        pan,
        tune,
        attack,
        length, // at its longest, the whole sound
        choke   // its choke group: off, or 1 to 4
    };

    static constexpr int fieldsPerPad = 6;
    static constexpr int outputParameter = numPads * fieldsPerPad;
    static constexpr int getParameterIndex (int pad, Field field) noexcept { return pad * fieldsPerPad + (int) field; }

    static const DeviceInfo info;

    // A sound a pad plays, at its own sample rate, in mono or stereo.
    struct Sound
    {
        juce::AudioBuffer<float> audio;
        double sampleRate = 48000.0;
    };

    // The default kit's sound for a pad, synthesized the first time it is needed, and its name ("Kick").
    static const Sound& getKitSound (int pad);
    static std::string_view getKitName (int pad);

    DrumSampler();
    ~DrumSampler() override;

    // Message thread. What a pad plays: a sound of its own, or with nullptr the kit's again. A sound the sampler no longer
    // plays is freed by a later call, once the audio thread can't be playing it any more.
    void setSound (int pad, std::unique_ptr<Sound>);
    bool hasOwnSound (int pad) const noexcept;

    // Plays a pad at the next block, as if its note came in: for the editor's pads. Message thread, lock-free.
    void audition (int pad, float velocity);

    // How hard each pad was hit since the editor last looked, from 0 to 1 (devices/core/MeterValue.h).
    MeterValue& getPadMeter (int pad) noexcept { return padMeters[(size_t) pad]; }

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    double getTailSeconds() const override;

private:
    struct Voice
    {
        int pad = -1; // none: the voice is free
        const Sound* sound = nullptr;
        double position = 0.0, step = 1.0;
        float gainLeft = 1.0f, gainRight = 1.0f;
        int played = 0, attackSamples = 0, lengthSamples = 0;
        float fade = 1.0f, fadeStep = 0.0f; // fading out once fadeStep is above 0
        std::uint64_t started = 0;
    };

    struct Retired
    {
        std::unique_ptr<Sound> sound;
        std::uint64_t safeAfter = 0; // blocks done after which the audio thread can't be playing it
    };

    double sampleRate = 44100.0;
    std::array<std::atomic<const Sound*>, numPads> sounds;
    std::array<Voice, maxVoices> voices {};
    std::uint64_t hits = 0;
    std::atomic<std::uint64_t> blocksDone { 0 };

    // The message thread's: the pads' own sounds, and those waiting to be freed.
    std::array<std::unique_ptr<Sound>, numPads> owned;
    std::vector<Retired> retired;

    static constexpr int auditionSlots = 64;
    juce::AbstractFifo auditionFifo { auditionSlots };
    std::array<std::pair<int, float>, auditionSlots> auditions {};

    std::array<MeterValue, numPads> padMeters;

    void hit (int pad, float velocity, std::span<const float> parameters);
    void render (float* left, float* right, int numSamples);
    void freeRetired();
};
} // namespace phazite::devices
