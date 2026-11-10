// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's own playback engine: it plays the plan the model builds from the song (Plan.h), handed to it whole, and the
// transport that the message thread asks for. Real-time safe on the audio thread: what it plays comes through a
// handover (Handover.h), what it is asked to do through a lock-free queue, and where it is goes out through values the
// message thread polls. Live, the model's playback host calls process() from the audio device's callback; a render
// makes an engine of its own and calls it in a loop (render()).
#pragma once

#include "EventQueue.h"
#include "Handover.h"
#include "Plan.h"
#include "PlayheadClock.h"
#include "Recording.h"
#include "Workers.h"
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
    void play (bool countIn = false);                   // from where the playhead is (after a count-in: below)
    void stop();                                        // where the playhead is
    void locate (double beat);                          // the playhead there, playing on from there if it plays
    bool isPlaying() const noexcept { return playing; } // as last asked

    // A count-in: a bar of clicks on the song's beats before the playhead, as long as a bar there, which the song waits
    // for; live notes and the inputs play meanwhile, and a take starts after it.

    // Where the playhead is: where the latest block the audio thread played started, moved on by the time since, as far as
    // a block and a half (any thread). Before the first block after a locate, where it was asked to be.
    double getPlayheadBeat() const noexcept;

    // Where the playhead was, or is, at a moment in milliseconds by juce::Time::getMillisecondCounterHiRes() (any thread):
    // in the block that was playing then, of the last few seconds' (PlayheadClock.h), each stamped as it started on a clock
    // smoothed over the blocks, so that the window's frames, each asking at its own time, see it move evenly however the
    // blocks come. Before the first block after a locate, where it was asked to be.
    double getPlayheadBeatAt (double milliseconds) const noexcept;

    // A note or controller played live into a track's instrument (the computer keyboard, a MIDI input, the piano roll's
    // guide notes), from any thread: it plays in the audio thread's next block, as far into it as it came after the last
    // block started, so that live notes keep their timing (at a block's delay). Dropped if 1024 wait already. What is
    // recordable comes out again as it was played (takePlayed()); the piano roll's guide notes aren't.
    void playLive (std::int64_t track, devices::NoteEvent, bool recordable = true) noexcept;

    // A note played live, as the engine played it: into a track, at the engine's clock as it came in (getClock()), and
    // where the song was then, or -1 while it was stopped or counting in.
    struct PlayedEvent
    {
        std::int64_t track = 0;
        devices::NoteEvent event {};
        std::int64_t clock = 0, songSample = -1;
    };

    // Message thread (one reader): the recordable notes played live since it last asked, as many as fit, oldest first;
    // how many. Once 4,096 wait, those played after them are lost.
    int takePlayed (std::span<PlayedEvent>) noexcept;

    // How many samples the engine has played since it was made, playing or not: its clock, as of its last block (any
    // thread).
    std::int64_t getClock() const noexcept { return publishedClock.load (std::memory_order_acquire); }

    // Message thread: what it records from its next block on (Recording.h), or nothing. What it replaces is freed once
    // the audio thread can't be using it.
    void setRecording (std::unique_ptr<Recording>);
    Recording* getRecording() const noexcept { return recordings.get(); }

    // How much of each block's time the engine takes, from 0 to 1, eased (any thread).
    double getLoad() const noexcept { return load.load (std::memory_order_relaxed); }

    // How the blocks came since this was last asked (any thread; the app asks a few times a second, for its log): those
    // that started more than two blocks' length after the one before (later than a shared mode's buffer hides: the
    // owner's crackles, 2026-11-02, tests/AudioGlitchTests.cpp), the longest time from one block's start to the next's
    // and the longest block the engine took, in seconds, and the last block's length.
    struct BlockTimes
    {
        int late = 0;
        double longestGap = 0.0, longestBlock = 0.0, blockLength = 0.0;
    };

    BlockTimes takeBlockTimes() noexcept;

    // How many blocks the audio thread has played: what it let go of before the count it read is no longer played
    // (audio/AudioFile.h frees what it lets go of after it).
    const std::atomic<std::uint64_t>& getBlocksPlayed() const noexcept { return blocksPlayed; }

    // Message thread, while the engine is stopped or not yet given to the audio device: so many threads play each block,
    // the audio thread one of them (Workers.h).
    void setNumThreads (int count) { workers.setNumThreads (count); }
    int getNumThreads() const noexcept { return workers.getNumThreads(); }

    // How loud what goes out is (any thread reads it, and resets its integrated loudness and held peak).
    LoudnessMeter& getLoudnessMeter() noexcept { return loudness; }

    // The audio thread: plays a block into the outputs (every channel the same), with what comes into the audio device's
    // inputs (by their channels, nullptr for one that is closed; the first 64), which the strips that hear them play and a
    // take records. Real-time safe.
    void process (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int numSamples) noexcept;
    void process (float* const* outputs, int numOutputs, int numSamples) noexcept { process (nullptr, 0, outputs, numOutputs, numSamples); }
    static constexpr int maximumInputs = 64;

    // A render (on the thread that calls it, not in real time): plays numSamples, from where the playhead is, playing,
    // into a stereo buffer that it makes, in blocks of the prepared size. The plan must have been given.
    juce::AudioBuffer<float> render (int numSamples);

    // A render in parts, as long as it takes (an export's, with its tails): begun from where the playhead is, playing,
    // ahead by as much as its sound comes out late; then so many samples at a time into two channels, each part going on
    // where the one before ended, in blocks of the prepared size; and ended. render() is one part.
    void beginRender();
    void renderPart (float* const* channels, int numSamples);
    void endRender();

    // Whether the blocks process() plays wait for what they play, as a render's do (a plug-in's block in its helper, with no
    // deadline; devices awake; audio clips read as they play), rather than keep to real time: for a test that plays blocks
    // faster than real time and must hear the same every time, however busy the machine is (tests/EnginePlayback.h). Never
    // on an audio device. Only while no block plays.
    void setPlayingAsRender (bool shouldWait) noexcept { playingAsRender = shouldWait; }

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
        bool countIn = false;
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
    // next stretch played. A count-in, while it lasts: its length and what is left of it in samples, the song's beat it
    // starts on, the first beat of the bar the playhead is in, and the beats of a bar and samples of a beat there.
    struct CountIn
    {
        std::int64_t length = 0, left = 0;
        double firstBeat = 0.0, barStart = 0.0;
        int beatsPerBar = 4;
        double samplesPerBeat = 1.0;
    };

    struct Transport
    {
        bool playing = false;
        std::int64_t sample = 0;
        double beat = 0.0;
        const Plan* plan = nullptr;
        bool notesOff = false, chase = true;
        bool flush = false; // playback starts or jumps: what rings on from before stops (flushStrip())
        CountIn countIn;
    } transport;

    bool rendering = false, playingAsRender = false; // a render's blocks, and a test's that wait as they do
    juce::AudioBuffer<float> renderBlock;            // the render's last block, of which the next part takes what is left
    int renderSkip = 0, renderTaken = 0, renderFilled = 0;

    // What comes into the audio device's inputs for the part of the block played, by their channels.
    std::array<const float*, maximumInputs> partInputs {};
    int numPartInputs = 0;

    // The take recorded, which ends as playback stops or jumps (brokeTake: since the last part).
    Handover<Recording> recordings;
    bool brokeTake = false;

    void takeCommands (const Plan&) noexcept;
    void playSpan (const Plan&, float* const* outputs, int numOutputs, int offset, int numSamples) noexcept;
    void playStrips (const Plan&, int offset, int numSamples) noexcept;

    // A stretch of a block that the strips play, which each strip's task reads; the strips' work on the audio thread and
    // the workers, each pass numbered, so that a strip knows when what it takes in is done. Played side by side, the
    // instruments handed their blocks early are handed them first, each a task (startStrip()), before the strips' tasks.
    struct Span
    {
        Engine* engine = nullptr;
        const Plan* plan = nullptr;
        int offset = 0, numSamples = 0;
        double beat = 0.0, tempo = 120.0, beatsPerSample = 0.0;
        std::int64_t gridSample = 0;
        std::uint64_t pass = 0;
        bool startsEarly = false, flush = false;
    };

    Workers workers;
    std::uint64_t passes = 0;
    static void findEarlyStarts (Plan&);
    void startStrip (const Span&, size_t index) noexcept;
    void playStrip (const Span&, size_t index) noexcept;
    void takeEvents (const Span&, const Plan::Strip&) noexcept; // its instrument's notes and live notes, then its modulators
    bool isSilent (const Plan::Strip&) noexcept;                // and its devices reset as it is heard again
    static void flushStrip (const Plan::Strip&) noexcept;       // its devices reset, and the delays that line it up
    static void resetContainers (const Plan::Strip&) noexcept;  // nothing its containers kept, nor their chains
    DeviceRunner::Block getDeviceBlock (const Span&, const Plan::Strip&, size_t device) noexcept; // without its notes

    // A strip's device played in a buffer (its strip's or its chain's), given its notes (an instrument's); and a container's
    // chains, side by side, each on its part of what the strip's buffer holds, added up into it. Whether a strip's first
    // device is an instrument container.
    void playDevice (const Span&, const Plan::Strip&, size_t device, juce::AudioBuffer<float>&,
                     std::span<devices::NoteEvent> notes) noexcept;
    void playContainer (const Span&, const Plan::Strip&, const Plan::Container&, float* const* channels) noexcept;
    static bool hasInstrumentContainer (const Plan::Strip&) noexcept;
    void addInput (const Plan::Strip&, float* const* channels, int offset, int numSamples) noexcept;
    void addNotes (const Plan::Strip&, const Plan&, int numSamples) noexcept;

    // The control points of a stretch of a block that modulators play on (ModulatorNode.h), the samples counted since the
    // engine started (its clock) numbering them while the song is stopped; each strip's modulators on them, and what they
    // move its fader by on each.
    std::vector<ModulatorNode::Point> controlPoints;
    int numControlPoints = 0;
    std::int64_t clock = 0;
    int partStart = 0;   // where the part of the block played starts in it (a block longer than a plan takes plays in parts)
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
        bool recordable = true;
    };

    EventQueue<LiveEvent, 1024> liveQueue;
    std::array<LiveEvent, 256> liveEvents {};
    int numLiveEvents = 0;
    double lastBlockMilliseconds = 0.0;
    void takeLiveEvents (const Plan&, int numSamples, std::int64_t partClock) noexcept;

    // The notes played live as they came out (takePlayed()), in a lock-free queue the audio thread alone fills; and the
    // last part played, from whose start the live notes taken in a part came in.
    static constexpr int maximumPlayed = 4096;
    juce::AbstractFifo playedFifo { maximumPlayed };
    std::vector<PlayedEvent> played;

    struct LastPart
    {
        std::int64_t clock = 0, sample = 0;
        bool playing = false;
    } lastPart;

    std::atomic<std::int64_t> publishedClock { 0 };

    std::atomic<double> load { 0.0 };
    std::int64_t lastBlockStart = 0; // the audio thread's, in high-resolution ticks; 0 before a first block
    std::atomic<std::int64_t> longestGapTicks { 0 }, longestBlockTicks { 0 };
    std::atomic<int> lateBlocks { 0 }, lastBlockLength { 0 };
    std::atomic<std::uint64_t> blocksPlayed { 0 };

    // The metronome: its two clicks (made in prepare()), and the one playing, where it is.
    juce::AudioBuffer<float> click, accentClick;
    const juce::AudioBuffer<float>* clicking = nullptr;
    int clickPosition = 0;
    void playClicks (const Plan&, float* const* outputs, int numOutputs, int offset, int numSamples) noexcept;
    void playCountIn (float* const* outputs, int numOutputs, int numSamples) noexcept;
    void continueClick (float* const* outputs, int numOutputs, int offset, int from, int to) noexcept;

    // Where the playhead was as each of the last blocks started, for any thread (PlayheadClock.h): stamped by the audio thread
    // with when the block started, on its blocks' smoothed clock, and how long it lasts (milliseconds).
    BlockClock blockClock;
    PlayheadStamps playheadStamps;
    void publishPlayhead (double beat, double beatsPerSecond, double startMilliseconds, double lengthMilliseconds) noexcept;

    Engine (const Engine&) = delete;
    Engine& operator= (const Engine&) = delete;
};
} // namespace phazite::engine
