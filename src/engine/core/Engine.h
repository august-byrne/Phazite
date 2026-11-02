// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's own playback engine (docs/tracktion-migration.md, T2): it plays the plan the model builds from the song
// (Plan.h), handed to it whole, and the transport that the message thread asks for. Real-time safe on the audio thread:
// what it plays comes through a handover (Handover.h), what it is asked to do through a lock-free queue, and where it is
// goes out through values the message thread polls. Live, the model's playback host calls process() from the audio
// device's callback; a render makes an engine of its own and calls it in a loop (render()).
#pragma once

#include "EventQueue.h"
#include "Handover.h"
#include "Plan.h"
#include "engine/nodes/LoudnessMeter.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace phazite::engine
{
class Engine
{
public:
    Engine();
    ~Engine();

    // Message thread, while process() doesn't run (before the audio device starts, or while it is stopped): the sample
    // rate, and the most samples process() is asked for at once (a longer block is played in parts). A plan is built for
    // the rate it plays at (Plan::tempo).
    void prepare (double sampleRate, int maximumBlockSize);
    double getSampleRate() const noexcept { return sampleRate; }
    int getMaximumBlockSize() const noexcept { return maximumBlockSize; }

    // Message thread: what it plays from its next block on; or nothing (silence).
    void setPlan (std::unique_ptr<Plan>);

    // Message thread, while no audio thread calls the engine (it is off): lets go of its plan, and of those it replaced,
    // at once.
    void dropPlans();
    const Plan* getPlan() const noexcept { return plans.get(); }

    // The transport, as the message thread asks for it: each takes effect at the start of the audio thread's next
    // block, in the order asked.
    void play();                                        // from where the playhead is
    void stop();                                        // where the playhead is
    void locate (double beat);                          // the playhead there, playing on from there if it plays
    bool isPlaying() const noexcept { return playing; } // as last asked

    // Where the playhead is: where the audio thread last played, moved on by the time since, as far as a block (any
    // thread). Before the first block after a locate, where it was asked to be.
    double getPlayheadBeat() const noexcept;

    // A note or controller played live into a track's instrument (the computer keyboard, a MIDI input, the piano roll's
    // guide notes), from any thread: it plays in the audio thread's next block, as far into it as it came after the last
    // block started, so that live notes keep their timing (at a block's delay). Dropped if 1024 wait already.
    void playLive (std::int64_t track, devices::NoteEvent) noexcept;

    // How much of each block's time the engine takes, from 0 to 1, eased (any thread).
    double getLoad() const noexcept { return load.load (std::memory_order_relaxed); }

    // How many blocks the audio thread has played: what it let go of before the count it read is no longer played
    // (audio/AudioFile.h frees what it lets go of after it).
    const std::atomic<std::uint64_t>& getBlocksPlayed() const noexcept { return blocksPlayed; }

    // How loud what goes out is (any thread reads it, and resets its integrated loudness and held peak).
    LoudnessMeter& getLoudnessMeter() noexcept { return loudness; }

    // The audio thread: plays a block into the outputs (every channel the same). Real-time safe.
    void process (float* const* outputs, int numOutputs, int numSamples) noexcept;

    // A render (on the thread that calls it, not in real time): plays numSamples, from where the playhead is, playing,
    // into a stereo buffer that it makes, in blocks of the prepared size. The plan must have been given.
    juce::AudioBuffer<float> render (int numSamples);

    // How late the master's sound comes out, in samples, for a plan: its devices' and its tracks' latency (delay
    // compensation), which a render plays ahead of so that its sound starts on time.
    static int getOutputLatency (const Plan&) noexcept;

private:
    double sampleRate = 44100.0;
    int maximumBlockSize = 512;
    Handover<Plan> plans;

    // What the message thread asks for, in a lock-free queue it alone fills and the audio thread alone empties.
    struct Command
    {
        enum class Type
        {
            play,
            stop,
            locate
        };

        Type type = Type::stop;
        double beat = 0.0;
    };

    static constexpr int maximumCommands = 64;
    juce::AbstractFifo commandFifo { maximumCommands };
    std::array<Command, maximumCommands> commands;
    bool playing = false; // the message thread's: as last asked
    void push (Command);

    // A locate asked for and not taken yet, whose beat the playhead shows meanwhile.
    std::atomic<double> requestedBeat { 0.0 };
    std::atomic<std::uint32_t> locatesAsked { 0 }, locatesTaken { 0 };
    static_assert (std::atomic<double>::is_always_lock_free);

    // The audio thread's transport: where the playhead is, in samples from the song's start, and in beats (to stay on its
    // beat when a plan with another tempo comes); and the plan it last played.
    // Whether every track's notes stop (playback stopped or jumped) and those sounding where it plays from start, at the
    // next stretch played.
    struct Transport
    {
        bool playing = false;
        std::int64_t sample = 0;
        double beat = 0.0;
        const Plan* plan = nullptr;
        bool notesOff = false, chase = true;
    } transport;

    bool rendering = false;

    void takeCommands (const Plan&) noexcept;
    void playSpan (const Plan&, float* const* outputs, int numOutputs, int offset, int numSamples) noexcept;
    void playStrips (const Plan&, int offset, int numSamples) noexcept;
    void addNotes (const Plan::Strip&, const Plan&, int numSamples) noexcept;

    // The control points of a stretch of a block that modulators play on (ModulatorNode.h), the samples counted since the
    // engine started (its clock) numbering them while the song is stopped; each strip's modulators on them, and what they
    // move its fader by on each.
    std::vector<ModulatorNode::Point> controlPoints;
    int numControlPoints = 0;
    std::int64_t clock = 0;
    Delay noDelay { 0 }; // joins what needs no delay, at a gain
    LoudnessMeter loudness;
    void findControlPoints (const Plan&, std::int64_t gridSample, int numSamples) noexcept;
    void playModulators (const Plan::Strip&) noexcept;
    std::span<const float> getFaderModulation (const Plan::Strip&, const std::vector<Plan::Route>&, std::vector<float>& offsets) noexcept;
    bool endsAtLoop = false; // the stretch played ends at the loop's end

    // Live notes: those waiting, from any thread; and those of the block played, each at its sample.
    struct LiveEvent
    {
        std::int64_t track = 0;
        devices::NoteEvent event {};
        double milliseconds = 0.0;
    };

    EventQueue<LiveEvent, 1024> liveQueue;
    std::array<LiveEvent, 256> liveEvents {};
    int numLiveEvents = 0;
    double lastBlockMilliseconds = 0.0;
    void takeLiveEvents (int numSamples) noexcept;

    std::atomic<double> load { 0.0 };
    std::atomic<std::uint64_t> blocksPlayed { 0 };

    // The metronome: its two clicks (made in prepare()), and the one playing, where it is.
    juce::AudioBuffer<float> click, accentClick;
    const juce::AudioBuffer<float>* clicking = nullptr;
    int clickPosition = 0;
    void playClicks (const Plan&, float* const* outputs, int numOutputs, int offset, int numSamples) noexcept;

    // Where the playhead is, for any thread: written by the audio thread after each block, between two counts of a
    // sequence (odd while it writes), so that a reader takes the three values from one block.
    std::atomic<std::uint32_t> playheadSequence { 0 };
    std::atomic<double> playheadBeat { 0.0 }, playheadMilliseconds { 0.0 }, beatsPerMillisecond { 0.0 };
    std::atomic<double> playheadLimit { 0.0 }; // how far it may move on before the next block: one block's beats
    void publishPlayhead (double beat, double beatsPerSecond, double blockSeconds) noexcept;

    Engine (const Engine&) = delete;
    Engine& operator= (const Engine&) = delete;
};
} // namespace phazite::engine
