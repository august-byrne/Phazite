// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's own playback engine (docs/tracktion-migration.md, T2): it plays the plan the model builds from the song
// (Plan.h), handed to it whole, and the transport that the message thread asks for. Real-time safe on the audio thread:
// what it plays comes through a handover (Handover.h), what it is asked to do through a lock-free queue, and where it is
// goes out through values the message thread polls. Live, the model's playback host calls process() from the audio
// device's callback; a render makes an engine of its own and calls it in a loop (render()).
#pragma once

#include "Handover.h"
#include "Plan.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

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

    // The audio thread: plays a block into the outputs (every channel the same). Real-time safe.
    void process (float* const* outputs, int numOutputs, int numSamples) noexcept;

    // A render (on the thread that calls it, not in real time): plays numSamples, from where the playhead is, playing,
    // into a stereo buffer that it makes, in blocks of the prepared size. The plan must have been given.
    juce::AudioBuffer<float> render (int numSamples);

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
    struct Transport
    {
        bool playing = false;
        std::int64_t sample = 0;
        double beat = 0.0;
        const Plan* plan = nullptr;
    } transport;

    void takeCommands (const Plan&) noexcept;
    void playSpan (const Plan&, float* const* outputs, int numOutputs, int offset, int numSamples) noexcept;

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
