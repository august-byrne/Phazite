// SPDX-License-Identifier: AGPL-3.0-or-later
// One of Phazite's own devices as an engine plays it (docs/tracktion-migration.md, T2): its engine-neutral DSP
// (src/devices/) with what every device gets around it, whichever engine plays it, Tracktion's (through the device's thin
// plug-in, model/devices/internal/DevicePlugin.h) or Phazite's own. It glides each continuous parameter from one block's
// value to the next and moves the modulated and automated ones itself, in stretches of a block, each starting on the
// song's control points, one every 32 samples from its start (docs/performance-audit.md, A7; docs/design.md, section 10),
// so that a song sounds the same at any block size; it applies an effect's mix, with what came in delayed as much as the
// effect delays its sound; it plays a mono channel in stereo; and it sleeps on silence (docs/performance-audit.md, A1).
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/DeviceScope.h"
#include "devices/core/Processor.h"
#include "devices/dsp/DelayLine.h"
#include "devices/modulation/Modulation.h"
#include "engine/automation/Curve.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace phazite::engine
{
// What modulates a device's parameters (roadmap A5, part 4; docs/design.md, section 10): each modulator or macro that
// moves one of them, and by how much. Built off the audio thread and handed to it whole (core/Handover.h); whoever plays the
// device sets each source's values for a block before it plays it.
struct Modulation
{
    // What moves parameters: a modulator, or a macro.
    struct Source
    {
        // Set for each block: whether it is on, and its value as the block starts; and for an LFO whose wave is set by the
        // point of its cycle alone, its wave, from where its cycle is as the block starts, moving so far in a sample. Or, in
        // Phazite's engine, its value on each control point of the block, from the one at or before its start
        // (modulation/ModulatorNode.h), which each stretch takes from the point it starts on.
        bool isOn = true;
        std::span<const float> points;
        float value = 0.0f;
        bool followsWave = false;
        devices::LfoShape shape;
        float phase = 0.0f;
        double cyclesPerSample = 0.0;

        // The runner's: its value as the last block started and as this one does, which it glides between.
        bool started = false;
        float previous = 0.0f, now = 0.0f;
    };

    // A source moving a parameter, by its index among the device's, by an amount from -1 to 1.
    struct Route
    {
        int parameter = 0;
        int source = 0;
        float amount = 0.0f;
    };

    std::vector<Source> sources;
    std::vector<Route> routes;
    std::vector<bool> isModulated; // by the parameter's index
};

// A MIDI message as a device's event at a sample, or nothing for the messages devices don't use. The channel doesn't matter.
std::optional<devices::NoteEvent> toNoteEvent (const juce::MidiMessage&, int sample);

class DeviceRunner
{
public:
    static constexpr int maximumEvents = 1024;                // more in one block are dropped
    static constexpr int smoothingStep = controlPointSpacing; // the stretch a parameter glides, is modulated and automated in

    DeviceRunner (const devices::DeviceInfo&, std::unique_ptr<devices::Processor>);

    const devices::DeviceInfo& getDeviceInfo() const noexcept { return info; }
    devices::Processor& getProcessor() noexcept { return *processor; }

    // What goes into it and comes out, after its mix, for its editor's displays (devices/core/DeviceScope.h).
    devices::DeviceScope& getScope() noexcept { return scope; }

    bool isInstrument() const noexcept { return info.kind == devices::DeviceKind::instrument; }
    int getLatencySamples() const noexcept { return latencySamples; } // once prepared
    double getTailSeconds() const { return processor->getTailSeconds(); }

    // Before playback starts, off the audio thread: it may allocate. Also where an effect's mix starts from.
    void prepare (double sampleRate, int maximumBlockSize);

    // Silences what sounds, and forgets the last block's values, so that the next block's start where they are.
    void reset();

    // A parameter its automation moves, by its index among the device's.
    struct Automated
    {
        int parameter = 0;
        const Curve* curve = nullptr;
    };

    // What a block plays besides its audio.
    struct Block
    {
        std::span<const float> values;        // each parameter's value as the block starts (a modulated one's without modulation)
        std::span<devices::NoteEvent> events; // in time order, each at its sample from the block's start: changed as it plays
        Modulation* modulation = nullptr;     // what modulates its parameters, its sources set for this block; or none
        double tempo = 120.0;                 // beats per minute at the block's start
        double beat = 0.0;                    // where the song is at the block's start, in beats
        bool isPlaying = false;               // whether the song plays
        bool isRendering = false;             // a render, which never sleeps: it plays everything as the song has it
        bool hasSidechain = false;            // the buffer's third and fourth channels hold the sidechain's sound

        // The parameters automation moves, in place of their values: while the song plays, each at its curve's value on
        // each control point; stopped, at its value where the song is, gliding there as a value set by hand does. Where
        // the automation is as the block starts, in beats and in samples (from which control points are counted): the
        // song's place, less how late the sound comes into the device, so that its automation lines up with its sound.
        std::span<const Automated> automation;
        double automationBeat = 0.0;
        std::int64_t sample = 0;
        double beatsPerSample = 0.0; // how far the song moves in a sample: 0 while it is stopped
    };

    // Plays a block: numSamples of the buffer from startSample, in place, on its first two channels (or its first alone,
    // played in stereo), any after them cleared. Real-time safe: no locks, no allocation.
    void process (juce::AudioBuffer<float>&, int startSample, int numSamples, const Block&);

    // Each parameter's value as the last block ended, and without its modulation (the audio thread's).
    std::span<const float> getValues() const noexcept { return currentValues; }
    std::span<const float> getBaseValues() const noexcept { return baseValues; }

private:
    const devices::DeviceInfo& info;
    std::unique_ptr<devices::Processor> processor;
    devices::DeviceScope scope;

    // Each parameter's value now (what the DSP reads), and before its modulation; as the block went and the last one did
    // (which a continuous one glides from, a stretch at a time); which are continuous, rather than choices and switches; each
    // one's range, in which modulation moves it; and each one's modulation in a stretch.
    std::vector<float> currentValues, targetValues, previousValues, baseValues;
    std::vector<bool> glides;
    std::vector<juce::NormalisableRange<float>> ranges;
    std::vector<float> offsets;
    bool hasPreviousValues = false;
    void keepValues() noexcept;
    void startModulation (Modulation&) noexcept;
    void setStretchValues (const Block&, bool automationMoves, int controlPoint, int stretch, int done, int length, int numSamples);

    juce::AudioBuffer<float> stereo; // for a mono channel: sized in prepare(), as long as the stretches played at once
    int latencySamples = 0;

    // An effect's mix (DeviceInfo.h, mixParameterID): what came in, delayed as much as the effect delays its sound.
    int mixIndex = -1;
    float appliedMix = 1.0f; // where the last stretch's ramp ended
    juce::AudioBuffer<float> dry;
    std::array<devices::dsp::DelayLine, 2> dryDelays;
    void mixWithDry (float* const* wet, int length, float targetMix);

    // Asleep: once what comes in and what it plays have been silent for sleepAfterSeconds (and longer than its latency),
    // with nothing to play, it skips its work until sound, a note or an event comes in, or something waits in the device to
    // play (devices::Processor::isWaitingToPlay(): a pad its editor played). Never in a render.
    static constexpr float silence = 1.0e-6f; // -120 dBFS
    static constexpr double sleepAfterSeconds = 0.25;
    int silentSamples = 0, sleepAfterSamples = 0;
    bool asleep = false;

    DeviceRunner (const DeviceRunner&) = delete;
    DeviceRunner& operator= (const DeviceRunner&) = delete;
};
} // namespace phazite::engine
