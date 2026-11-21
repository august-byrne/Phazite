// SPDX-License-Identifier: AGPL-3.0-or-later
// One of Phazite's own devices, or a plug-in in its helper, as Phazite's engine plays it (DeviceNode.h): its
// engine-neutral DSP (src/devices/, or the plug-in's channel: model/plugins/internal/SandboxedProcessor.h) with what
// every device gets around it. It glides each continuous parameter from one block's value to the next and moves the
// modulated and automated ones itself, in stretches of a block, each starting on the song's control points, one every
// 32 samples from its start (docs/archive/performance-audit.md, A7; docs/design.md, section 10), so that a song sounds the same
// at any block size (a device that takes its blocks whole, a plug-in in its helper, gets a block in one call, with its
// moved parameters' values on those points as changes at their samples, and an instrument of them may be handed it
// early, to play beside the others: start()); it applies an effect's mix, with what came in delayed as much as the
// effect delays its sound and turned as the effect turns its phase (devices::ProcessContext::dry); it plays a mono
// channel in stereo; and it sleeps on silence (docs/archive/performance-audit.md, A1).
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/DeviceScope.h"
#include "devices/core/Processor.h"
#include "devices/dsp/DelayLine.h"
#include "devices/modulation/Modulation.h"
#include "engine/automation/Curve.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
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
    std::vector<int> modulated;    // the parameters modulated, by their indices, each once
};

// A MIDI message as a device's event at a sample, on its channel, or nothing for the messages devices don't use. All notes
// off and all sound off, on any channel, are every channel's.
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
        bool isPlaying = false;               // whether the song plays
        bool isRendering = false;             // a render, which never sleeps: it plays everything as the song has it
        bool hasSidechain = false;            // the buffer's third and fourth channels hold the sidechain's sound

        // Where the song is at the block's start: its tempo there, in beats (quarter notes) a minute; its place, in beats from
        // its start, in bars and in samples; and its time signature there, with how long its bar is and where the bar the
        // song is in starts, in beats (devices::ProcessContext's).
        double tempo = 120.0, beat = 0.0, bars = 0.0;
        std::int64_t songSample = 0;
        int timeSignatureNumerator = 4, timeSignatureDenominator = 4;
        double barLength = 4.0, barStart = 0.0;

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
        double beatsPerSample = 0.0, barsPerSample = 0.0; // how far the song moves in a sample, in each: 0 while it is stopped
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

    // How many blocks it has played since it was made, those it slept through not counted (any thread): what a test counts
    // to see it rest.
    std::uint64_t getBlocksWorked() const noexcept { return blocksWorked.load (std::memory_order_relaxed); }

private:
    const devices::DeviceInfo& info;
    std::unique_ptr<devices::Processor> processor;
    devices::DeviceScope scope;

    // Each parameter's value now (what the DSP reads), and before its modulation; as the block went and the last one did
    // (which a continuous one glides from, a stretch at a time); which are continuous, rather than choices and switches; each
    // one's range, in which modulation moves it; and each one's modulation in a stretch.
    std::vector<float> currentValues, targetValues, previousValues, baseValues;
    std::vector<bool> glides;
    std::vector<devices::Travel> travels;
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
        int ramp = 0; // from its start to its control point: as long, or longer where the block's end cuts it
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
        std::span<devices::NoteEvent> events; // what the device is handed of the block's events (mergeChannels())
    };

    Begun begin (juce::AudioBuffer<float>&, int start, int numSamples, const Block&);
    void end (juce::AudioBuffer<float>&, int start, int numSamples, const Block&, const Begun&);
    devices::ProcessContext makeContext (const Block&) const noexcept;
    bool startedEarly = false;
    Begun early;

    juce::AudioBuffer<float> stereo; // for a mono channel: sized in prepare(), as long as the stretches played at once
    int latencySamples = 0;
    double sampleRate = 44100.0;

    // An effect's mix (DeviceInfo.h, mixParameterID): what came in, turned as the effect turns its phase, then delayed as
    // much as the effect delays its sound, unless the effect delays and turns it itself (Processor::delaysDry()).
    int mixIndex = -1;
    float appliedMix = 1.0f; // where the last stretch's ramp ended, or reached where a block's end cut it
    juce::AudioBuffer<float> dry;
    std::array<devices::dsp::DelayLine, 2> dryDelays;
    // The dry from so far into its buffer, the mix ramped to its target over `ramp` samples of which these are the first
    // `length` (a stretch a block's end cuts goes on ramping in the next block), so that it plays the same in any blocks.
    void mixWithDry (float* const* wet, int dryStart, int length, float targetMix, int ramp);

    // Asleep: once what comes in and what it plays have been silent for sleepAfterSeconds (and longer than its latency,
    // and as long as the tail it reports then: devices::Processor::getTailSeconds()), with nothing to play, it skips its
    // work until sound, a note or an event comes in, or something waits in the device to play
    // (devices::Processor::isWaitingToPlay(): a pad its editor played); or, for a device that a change of its values alone
    // can make sound (devices::Processor::wakesWhenValuesChange(): the Filter), until the values it would be given in a
    // block, on any of its control points, automation and modulation applied, differ from those it was given last before it
    // fell asleep and the device would sound for them (sleptValues, devices::Processor::wakesFor(); known bug 203 until
    // 2026-11-18): woken so, for the whole block, it stays awake as long as the
    // rules here say, as if sound had come in. A device that doesn't ask is never compared. Never in a render, nor an
    // instrument while it holds
    // a note or its sustain pedal is down: a held note can be silent for a while and sound again (a slow LFO on its filter,
    // a sample with a gap), and asleep it would stay silent (known bug 37 until 2026-11-14). While the song is stopped, its
    // tail is waited for only once something that can sound has come in since it was prepared or reset (heard: a note's
    // start, sound, or what waits in it to play; not an all-notes-off, a note's end, a bend, a slide, pressure or a
    // controller alone): until then it
    // holds nothing to ring with (a device given nothing plays nothing, stopped or playing, from prepare() or a reset(): the
    // conformance suite's rule), so a reverb that hasn't heard anything rests after sleepAfterSeconds rather than its decay:
    // as a project opens, or a device is added or switched on while the song is stopped. A device is reset by Play, by a
    // jump while the song plays (Engine::flushStrip()), by being switched on (DeviceNode), and prepared by prepare(); a jump
    // while the song is stopped resets nothing, so a tail ringing before it rings on, and is waited for. While
    // the song plays, it waits for its tail all the same, so that a device whose sound starts within that tail plays it as a
    // render does (a render never sleeps): its inner motion, which runs on through silence (a reverb's lines' sway), would
    // stand still asleep, and a send to a reverb that begins a second in would sound otherwise live (the golden mix). One
    // that plays with the song (devices::Processor::playsWithTheSong(): Age's noise) sleeps only while it is stopped,
    // and wakes as it plays.
    static constexpr float silence = 1.0e-6f; // -120 dBFS
    static constexpr double sleepAfterSeconds = 0.25;
    static constexpr int maximumSilentSamples = 1 << 30; // counted no further: longer than any tail
    int silentSamples = 0, sleepAfterSamples = 0;
    bool asleep = false, heard = false;
    std::vector<float> sleptValues, valuesNow; // sized in prepare(), for a device that wakes when its values change
    bool valuesMovedInSleep (const Block&, int numSamples) noexcept;
    std::atomic<std::uint64_t> blocksWorked { 0 }; // written by the audio thread alone
    static_assert (std::atomic<std::uint64_t>::is_always_lock_free);
    // How many times it holds each note, on any channel (the song's and a key played live can hold the same one), how many
    // in all, and the channels whose pedal is down (a bit for each).
    std::array<std::uint8_t, 128> held {};
    int numHeld = 0;
    std::uint16_t pedalsDown = 0;
    void followNotes (std::span<const devices::NoteEvent>) noexcept;
    void letGoOfNotes() noexcept;

    // For an instrument that takes every MIDI channel as one (devices::Processor::playsChannelsApart() false), the channels
    // that hold each pitch (a bit for each: set by a note's start, cleared by its end on that channel, all cleared by an
    // all-notes-off and a reset), and the block's events as it is handed them, made once: a note's end only where no other
    // channel still holds its pitch, so that a note on one channel doesn't end the same pitch held on another. One channel
    // alone is handed every event as it comes.
    std::array<std::uint16_t, 128> channelsHolding {};
    std::vector<devices::NoteEvent> merged;
    std::span<devices::NoteEvent> mergeChannels (std::span<devices::NoteEvent>) noexcept;

    DeviceRunner (const DeviceRunner&) = delete;
    DeviceRunner& operator= (const DeviceRunner&) = delete;
};
} // namespace phazite::engine
