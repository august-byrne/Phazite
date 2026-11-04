// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's drum sampler (roadmap step 1.6): 16 pads on the notes from 36 up, General MIDI's drums, as FL Studio's FPC
// and Ableton's Drum Rack lay them out. Each pad plays a sound once, whole, however short the note (a one-shot), at its
// level, pan and tuning, with an attack that softens its start and a length that cuts it short. Pads in the same choke
// group stop each other, as an open hat stops when the closed one plays. A new sampler plays the default kit of the
// sounds Phazite synthesizes (FactoryDrums.h); a pad can play another of them, from the kits that come with Phazite, or a
// sound file, which the model loads (model/devices/internal/DrumKit.h), and which the sampler swaps in without the
// audio thread ever waiting or freeing memory.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/LiveValue.h"
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

    DrumSampler();
    ~DrumSampler() override;

    // Message thread. What a pad plays: a sound given it (a factory sound, or a file's), or with nullptr its default, the
    // default kit's (FactoryDrums.h). A sound the sampler no longer plays is let go by a later call, once the audio thread
    // can't be playing it any more.
    void setSound (int pad, std::shared_ptr<const Sound>);
    bool hasOwnSound (int pad) const noexcept; // given one, rather than playing its default

    // Plays a pad at the next block, as if its note came in: for the editor's pads. Message thread, lock-free. Played while
    // the sampler plays no blocks (switched off, or on a muted or frozen track), it never plays: one that waited longer
    // than longestAuditionWait is dropped, rather than heard late with every pad played since.
    void audition (int pad, float velocity);
    bool isWaitingToPlay() const noexcept override; // a pad played, for the next block

    // How hard each pad was hit since the editor last looked, from 0 to 1 (devices/core/MeterValue.h).
    MeterValue& getPadMeter (int pad) noexcept { return padMeters[(size_t) pad]; }

    // Message thread: the sound a pad plays now, for drawing it.
    const Sound& getPadSound (int pad) const noexcept { return *playing[(size_t) pad]; }

    // Where each pad's newest hit is in its sound, from 0 to 1, or below 0 while it plays none (devices/core/LiveValue.h).
    const LiveValue& getPlayhead (int pad) const noexcept { return playheads[(size_t) pad]; }

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
        std::shared_ptr<const Sound> sound;
        std::uint64_t safeAfter = 0; // blocks done after which the audio thread can't be playing it
    };

    double sampleRate = 44100.0;
    std::array<std::atomic<const Sound*>, numPads> sounds;
    std::array<Voice, maxVoices> voices {};
    std::uint64_t hits = 0;
    std::atomic<std::uint64_t> blocksDone { 0 };

    // The message thread's: what each pad plays, whether it was given that, and the sounds let go of, held until the
    // audio thread can't be playing them.
    std::array<std::shared_ptr<const Sound>, numPads> playing;
    std::array<bool, numPads> given {};
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
    void render (float* left, float* right, int numSamples);
    void freeRetired();
};
} // namespace phazite::devices
