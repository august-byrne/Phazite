// SPDX-License-Identifier: AGPL-3.0-or-later
// One of Phazite's own devices, or a plug-in in its helper, as Phazite's engine plays it (DeviceNode.h): its
// engine-neutral DSP (src/devices/, or the plug-in's channel: model/plugins/internal/SandboxedProcessor.h) with what
// every device gets around it. It glides each continuous parameter from one block's value to the next and moves the
// modulated and automated ones itself, in stretches of a block, each starting on the song's control points, one every
// 32 samples from its start (docs/performance-audit.md, A7; docs/design.md, section 10), so that a song sounds the same
// at any block size (a device that takes its blocks whole, a plug-in in its helper, gets a block in one call, with its
// moved parameters' values on those points as changes at their samples, and an instrument of them may be handed it
// early, to play beside the others: start()); it applies an effect's mix, with what came in delayed as much as the
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
        // Set for each block: whether it is on, and its value on each control point of the block, from the one at or
        // before its start (modulation/ModulatorNode.h), which each stretch takes from the point it starts on. Without
        // those, its value as the block starts.
        bool isOn = true;
        std::span<const float> points;
        float value = 0.0f;

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
    static constexpr int maximumChanges = 4096;               // for a device that takes its blocks whole; more are dropped
    static constexpr int smoothingStep = controlPointSpacing; // the stretch a parameter glides, is modulated and automated in

    DeviceRunner (const devices::DeviceInfo&, std::unique_ptr<devices::Processor>);

    const devices::DeviceInfo& getDeviceInfo() const noexcept { return info; }
    devices::Processor& getProcessor() noexcept { return *processor; }
    const devices::Processor& getProcessor() const noexcept { return *processor; }

    // What goes into it and comes out, after its mix, for its editor's displays (devices/core/DeviceScope.h).
    devices::DeviceScope& getScope() noexcept { return scope; }

    bool isInstrument() const noexcept { return info.kind == devices::DeviceKind::instrument; }
    int getLatencySamples() const noexcept { return latencySamples; } // once prepared
    double getSampleRate() const noexcept { return sampleRate; }      // once prepared
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

        // Whether a value set by hand may have changed since the last block (the device's node knows; true where it isn't known).
        bool valuesChanged = true;

        // Outputs of their own that some of its parts play into (devices::ProcessContext::partOutputs), each from outputOffset,
        // where the block starts in them.
        std::span<const devices::PartOutput> partOutputs;
        int outputOffset = 0;

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

    // Starts a block early, for an instrument that is handed its blocks so (devices::Processor::startsEarly()), without a
    // sidechain: as process() would hand it over, before what comes before it in the engine has played, so that it plays
    // meanwhile. The next process(), of the same block, then only takes what it played into its channels. Real-time safe.
    void start (juce::AudioBuffer<float>&, int startSample, int numSamples, const Block&);

    // Each parameter's value as the last block ended, and without its modulation (the audio thread's).
    std::span<const float> getValues() const noexcept { return currentValues; }
    std::span<const float> getBaseValues() const noexcept { return baseValues; }

    // Whether the last block found nothing moved since the one before (no value set by hand, no automation, modulation,
    // glide or stretch in either): its values were as they had been, and went untouched (the audio thread's).
    bool isSettled() const noexcept { return lastSettled; }

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
    bool lastMoved = true, lastSettled = false; // the last block moved its values (automation, a glide, a stretch); or settled
    void keepValues() noexcept;
    void startModulation (Modulation&) noexcept;
    void setStretchValues (const Block&, bool automationMoves, int controlPoint, int stretch, int done, int length, int numSamples,
                           const std::vector<int>* only = nullptr); // those alone, or every parameter

    // For a processor that takes its blocks whole (devices::Processor::takesWholeBlocks(), which a plug-in can have
    // thousands of parameters for): the parameters automation or modulation moves in this block and in the last, by their
    // indices, with the block each was last moved in; those it works out the stretches' values of (with the mix, if that
    // moves); the values it last sent; and a chunk's changes, with each of its stretches' mix.
    struct StretchMix
    {
        int start = 0, length = 0;
        float mix = 1.0f;
    };

    std::vector<int> movedNow, movedBefore, stretchParameters;
    std::vector<std::uint64_t> movedIn;
    std::uint64_t blocksStarted = 0;
    std::vector<float> sentValues;
    std::vector<devices::ParameterChange> changes;
    std::vector<StretchMix> stretchMixes;
    void startWholeBlock (const Block&, bool isMixMoved) noexcept;
    int gatherChanges (const Block&, bool automationMoves, bool isMixMoved, int sinceControlPoint, int stretch, int from, int length,
                       int numSamples); // the next stretch's index

    // What a block's start finds (begin()), for its end (end()); and a block started early (start()), whose process() only
    // takes what it played.
    struct Begun
    {
        bool slept = false;   // asleep, it played nothing
        bool settled = false; // nothing moved since the last block: its values as they were, untouched
        bool automationMoves = false, inStretches = false, isMixMoved = false, hasSidechain = false, quietIn = false;
        float targetMix = 1.0f;
    };

    Begun begin (juce::AudioBuffer<float>&, int start, int numSamples, const Block&);
    void end (juce::AudioBuffer<float>&, int start, int numSamples, const Block&, const Begun&);
    devices::ProcessContext makeContext (const Block&) const noexcept;
    bool startedEarly = false;
    Begun early;

    juce::AudioBuffer<float> stereo; // for a mono channel: sized in prepare(), as long as the stretches played at once
    int latencySamples = 0;
    double sampleRate = 44100.0;

    // An effect's mix (DeviceInfo.h, mixParameterID): what came in, delayed as much as the effect delays its sound.
    int mixIndex = -1;
    float appliedMix = 1.0f; // where the last stretch's ramp ended
    juce::AudioBuffer<float> dry;
    std::array<devices::dsp::DelayLine, 2> dryDelays;
    void mixWithDry (float* const* wet, int dryStart, int length, float targetMix); // dry from so far into its buffer

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
