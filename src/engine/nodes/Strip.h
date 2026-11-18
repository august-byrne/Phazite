// SPDX-License-Identifier: AGPL-3.0-or-later
// A track, a bus, a return or the master as Phazite's engine plays it, kept by its ID from one plan to the next: its
// channel (Channel.h), the buffer it plays into, what it sends before its fader and what a sidechain hears of it, and
// where its notes are as it plays them. And what joins paths: the delay that lines a path up with a later one (delay
// compensation), and a send's level. (A container's chains, and its mix of their sound with what came in: Container.h.)
#pragma once

#include "Channel.h"
#include "DeviceRunner.h"
#include "NoteSequence.h"
#include "engine/automation/Curve.h"
#include "engine/modulation/ModulatorNode.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace phazite::engine
{
class StripState
{
public:
    // Message thread, before a plan points at it: buffers for blocks of up to so many samples.
    void prepare (double sampleRate, int maximumBlockSize);

    Channel& getChannel() noexcept { return channel; }

    // The audio thread's: what it plays, on two channels, and on two more what a device's sidechain hears; its sound before
    // its fader, for sends before the fader; its sound after its fader and before its mute, for sidechains and for sends
    // after the fader; and what its audio clips read (AudioRegion::play()).
    juce::AudioBuffer<float> buffer, beforeFader, beforeMute, scratch;

    // Where its notes are: the sequence played last, its next event, the pitches sounding, and whether the notes sounding
    // where it plays from must start (after a jump, the loop's start, or playback starting).
    const NoteSequence* sequence = nullptr;
    std::size_t nextEvent = 0;
    std::bitset<128> sounding;
    bool mustChase = true;

    // What its instrument plays in a stretch of a block, in time order.
    std::array<devices::NoteEvent, DeviceRunner::maximumEvents> events {};
    int numEvents = 0;
    void addEvent (devices::NoteEvent) noexcept; // dropped once full
    void sortEvents() noexcept;

    // What its modulators hear of its notes in a block, the notes started; and its fader's modulation on each of the block's
    // control points (Channel::Automation).
    std::vector<ModulatorNode::NoteStart> noteStarts;
    std::vector<float> volumeModulation, panModulation;

    // For what is sent to it, one send at a time: a send's level on each of a block's samples, and its modulation on each
    // of the block's control points (SendLevel).
    std::vector<float> sendGains, sendModulation;

    // The engine's pass it has played last (Engine.h), which the strips that take it in wait for; and for one whose
    // instrument is handed its blocks early, the pass whose block it was last handed, which the strip's own task waits for.
    std::atomic<std::uint64_t> donePass { 0 }, startedPass { 0 };

private:
    Channel channel;
};

// What came out of a strip at a point (its output, its sound before its fader or before its mute, a device's part), or
// into one path that a delay lines up with a later one, on two channels: a ring of so many samples, kept from plan to plan
// while it is long enough for what reads it, so that a delay made longer or shorter (a device's latency changed) plays on
// from what came in before, rather than from silence (known bug 167). Every path into a sum reads one, delayed or not,
// so that one that comes to need a delay has its past; the paths from one point of a strip (the sends of a track to its
// returns) share one, which that strip writes once a block (a tap: Plan::Strip::taps). One made in place of a ring too
// short copies nothing: it reads the part of the past from before it started from the ring it takes the place of (the
// latest that played), which stays as it was, until it holds as much of the past as it has room for itself, and is then
// done with it; so the work of a block is bounded by its length, whatever the rings' sizes. The ring it takes the place
// of stays alive as long as this one needs it, through the chain of rings each holding the one before, which the message
// thread lets go of once this one is done with it.
class DelayLine
{
public:
    // Message thread: room for so many samples, holding nothing yet; or with what the ring it takes the place of holds. Its
    // room is made at once, or later (allocate()) for one made as a plan is built, of which the plan may need none.
    explicit DelayLine (int capacity, std::shared_ptr<DelayLine> takesFrom = {}, bool allocateNow = true);
    ~DelayLine(); // the message thread's: the rings it holds let go of one after another

    int getCapacity() const noexcept { return capacity; }

    // Message thread, before a plan points at it: its room made, silent, if it wasn't.
    void allocate();
    bool isAllocated() const noexcept { return samples.getNumSamples() == capacity; }

    // Message thread: the ring it takes the place of let go of, once it is done with it.
    void letGoOfTheOld() noexcept;

    // Audio thread: as it is first played, it takes the latest ring before it that played (one that never played holds
    // nothing of its own) as what it reads the past from, as far as that holds it.
    void start() noexcept;

    // Audio thread: silence in it, all of it heard (as after a reset, which starts the song from silence); done with the
    // ring before. Its memory isn't touched: it holds nothing of its own yet, and what is older than what it holds is
    // silence (forEachHeld() hands none of it), as far back as its capacity (getReach()), so that a delayed path reads
    // silence there rather than nothing, which would fade it in. So a reset costs the same whatever the ring's size: every
    // Play and every jump resets every ring (known bug 85's review: it cleared each whole, about 53 MB at 48 kHz on a song of
    // 200 tracks sending to 8 returns, 210 MB at 192 kHz, in the block that starts or lands).
    void reset() noexcept;

    // Audio thread: how many samples of the past it holds, its own and what it reads from the ring before; since a reset,
    // all of its capacity, silent before what it has kept.
    int getReach() const noexcept { return silentBefore ? capacity : written + (done.load (std::memory_order_relaxed) ? 0 : inherited); }

    // For tests: how many samples every ring has cleared (written as silence), on two channels, since the program started:
    // each as its room is made, and nothing since.
    static std::int64_t getSamplesCleared() noexcept { return clearedSamples.load (std::memory_order_relaxed); }

    // Audio thread: what came in, kept, so many samples.
    void write (const float* const* inputs, int numSamples) noexcept;

    // Audio thread: what it holds of so many samples, the first so many samples back from where the next goes (ago, at
    // least count), each run of them handed to a function with where it is among them: (offset, left, right, length). What
    // it doesn't hold (further back than it reaches) is handed to none. Its own samples first, then the older ones from the
    // ring before, then that one's, as far as each reaches: never more samples than asked, wherever they are.
    template <typename Take>
    void forEachHeld (int ago, int count, Take&& take) const noexcept
    {
        for (const auto* ring = this; ring != nullptr && count > 0;
             ring = ring->done.load (std::memory_order_relaxed) ? nullptr : ring->source)
        {
            const auto ownFrom = std::clamp (ago - ring->written, 0, count); // the first it holds itself

            for (int at = ownFrom; at < count;)
            {
                const auto from = ((ring->position - (ago - at)) % ring->capacity + ring->capacity) % ring->capacity;
                const auto part = std::min (count - at, ring->capacity - from);
                take (at, ring->samples.getReadPointer (0, from), ring->samples.getReadPointer (1, from), part);
                at += part;
            }

            // What is older than its own, from the ring before.
            ago -= ring->written;
            count = ownFrom;
        }
    }

private:
    juce::AudioBuffer<float> samples; // the audio thread's
    const int capacity;
    int position = 0, written = 0; // where the next sample goes, and how many it has kept itself (up to its capacity)

    // The ring it takes the place of, until it is done with it; and the latest before it that played, which it reads the
    // past from (alive through old: each ring holds the one before until it is done with it), with how much of the past
    // that one held as this one started.
    std::shared_ptr<DelayLine> old;
    const DelayLine* source = nullptr;
    int inherited = 0;

    // The audio thread's: started once played; done once it holds as much as it has room for itself, or after a reset, when
    // the message thread lets go of the old.
    std::atomic<bool> started { false }, done { false };

    // The audio thread's: reset, and not yet written all the way round since: what it hasn't kept itself is silence.
    bool silentBefore = false;

    static inline std::atomic<std::int64_t> clearedSamples { 0 };
};

// A delay of a fixed length on two channels, reading a ring (DelayLine) so many samples back: one of its own, which it
// writes what comes in into itself; or a strip's tap, which that strip has written the stretch into before (tapped), read
// from as far back from its end. Until the ring holds that much of the past (a new path, or one made longer than what its
// ring held), it plays nothing, and then fades in over so many samples, rather than cut in mid-sound; a path new to a song
// that plays fades in from its first sample, though it has no delay.
class Delay
{
public:
    explicit Delay (int samples); // a ring of its own, as long, holding silence
    Delay (std::shared_ptr<DelayLine>, int samples, int fadeSamples, bool newPath = false, bool tapped = false);
    ~Delay(); // the message thread's: the delays before it let go of one after another

    int getLength() const noexcept { return length; }
    const std::shared_ptr<DelayLine>& getLine() const noexcept { return line; }
    bool isTapped() const noexcept { return tapped; }

    // Message thread, before a plan points at it: the delay it takes the place of on its path (one of another length), held
    // until it has played; and let go of once it has.
    void takePlaceOf (std::shared_ptr<Delay> before) noexcept { previous = std::move (before); }
    void letGoOfThePrevious() noexcept;

    // Message thread, before a plan points at it: what it reads held what a path that left added to a sum (it reads a
    // return or a bus such a path went into, or what that one went into), so that what it would play again holds that.
    void markReadsLeaving() noexcept { readsLeaving.store (true, std::memory_order_relaxed); }

    // Audio thread, before it plays: how much further back it reads than the latest delay before it on its path that
    // played, where it or one of the delays before it that didn't play was marked as reading what left (0 otherwise, for
    // one that played itself, or a path new to the song): what a plan that takes its place plays again of what left.
    int getGrowth() const noexcept;

    // Audio thread: adds what came in so many samples ago to the outputs, at a gain going in a straight line from one to
    // another over the block, times each sample's own gain if given (a send that follows its automation: SendLevel); and
    // keeps what comes in now, in its own ring (a tapped one's is written by its strip, and what comes in isn't read).
    void addDelayed (const float* const* inputs, float* const* outputs, int numSamples, float startGain = 1.0f, float endGain = 1.0f,
                     const float* gains = nullptr) noexcept;
    void reset() noexcept; // audio thread: silence in it, as when it was made (a tapped one's ring is its strip's to reset)

    // Message thread: the path goes with the plan of these departures (engine/core/Plan.h: departures); any thread: which,
    // 0 for none.
    void setDeparture (std::uint64_t departures) noexcept { departure.store (departures, std::memory_order_relaxed); }
    std::uint64_t getDeparture() const noexcept { return departure.load (std::memory_order_relaxed); }

    // Audio thread, on the plan it leaves from (Engine.h: Leave): its gains for a stretch of so many samples, fading out
    // over so many from the first stretch the engine heard it leave in (it starts its fade then, whenever its mark came),
    // and silent after, as long as that plan plays.
    std::pair<float, float> leave (int fadeLength, int numSamples, std::pair<float, float> gains) noexcept;

    // How many samples its last block read from rings and wrote into its own, on one channel: its work, which a test holds
    // to the block's length.
    int getSamplesTouched() const noexcept { return touched; }

private:
    int length = 0, fadeSamples = 0;
    std::shared_ptr<DelayLine> line;
    bool tapped = false;
    std::atomic<std::uint64_t> departure { 0 };

    // The delay it took the place of, until it has played (set by the audio thread as it first plays, which alone reads the
    // delays before through it, and only while it hasn't).
    std::shared_ptr<Delay> previous;
    std::atomic<bool> played { false }, readsLeaving { false };

    // The audio thread's: how far into its fade in it is (-1: none to make, or done), whether it is a path new to the song
    // and has played, what is left of its fade out as it leaves (-1 before it does), and its last block's work.
    int fadedIn = -1;
    bool newPath = false, started = false;
    int leaving = -1;
    int touched = 0;
};

// A send's level, kept by the send's ID: set by hand, which the message thread sets and the audio thread glides to over a
// block; or, while the song plays, its automation and its track's modulation of it, which it follows as a fader follows its
// volume's (Channel.h): along the fader's travel (its knob's and its lane's, Channel.h: faderPositionToDecibels()), at their
// values on the song's control points and in a straight line between, from where the line was as a block started, whatever
// the block's size. Stopped, it glides to its automation's value where the song is, as to a level set by hand. As it stops
// following (its lane deleted while the song plays, a pass of Write begun, Stop), it eases to where it goes over 15 ms at
// least, as a fader does, rather than in a block: at 64 samples a block, a lane at -30 dB deleted under a send set to 0 dB
// went up 30 dB in 1.3 ms. Cut, it sends nothing whatever its level, gliding there over a block too (the model says when: a
// muted track's sends after its fader, and every send of a track another's solo silences but one to a return that is
// soloed).
class SendLevel
{
public:
    // Message thread: the level set by hand.
    void setDecibels (float decibels) noexcept;
    void setCut (bool shouldCut) noexcept { cut.store (shouldCut, std::memory_order_relaxed); }
    bool isCut() const noexcept { return cut.load (std::memory_order_relaxed); }
    float getGain() const noexcept { return gain.load (std::memory_order_relaxed); } // as set (the tests' comparisons)

    // Message thread: whether its level is written by hand now (automation's touch, latch and write modes), so that it plays
    // its level set by hand rather than its automation.
    void setWriting (bool isWriting) noexcept { writing.store (isWriting, std::memory_order_relaxed); }

    // Any thread: the level it played last, in the fader's positions, its automation's or the one set by hand, with its
    // modulation or without; below 0 before it has played.
    float getPlayedPosition (bool withModulation = true) const noexcept
    {
        return (withModulation ? playedPosition : playedBasePosition).load (std::memory_order_relaxed);
    }

    // A block's automation of the level, in the fader's positions (or none), where the song is for it (as late as the sound
    // it sends comes into its return) and where the song is: while it plays (beatsPerSample above 0) it follows it; and its
    // modulation, as parts of the fader's travel on each control point from the one at or before the block's start (none for
    // none), which moves it from there, as Channel::Automation's volume.
    struct Automation
    {
        const Curve* curve = nullptr;
        double beat = 0.0;
        double beatsPerSample = 0.0; // 0 while the song is stopped
        std::int64_t sample = 0;     // the song's sample at the block's start
        std::span<const float> modulation;
        int easeSamples = 0; // what leaving its automation for another level is spread over at least: 15 ms at the rate
    };

    static constexpr double easeSeconds = 0.015;

    // Its gain over a block: a straight line from one to the other; and while it follows its automation or modulation
    // (followed), each sample's level too, in the gains it was given, which the line (its cut's glide) then turns.
    struct Gains
    {
        float start = 1.0f, end = 1.0f;
        bool followed = false;
    };

    // Audio thread: its gains over a block of as many samples as it is given room for. Given none (a block longer than its
    // strip was prepared for), it neither follows nor eases: it glides to the level set by hand over the block.
    Gains moveOn (const Automation&, std::span<float> gains) noexcept;

private:
    std::atomic<float> gain { 1.0f }, position { decibelsToFaderPosition (0.0f) }; // set by hand: 0 dB
    std::atomic<float> playedPosition { -1.0f }, playedBasePosition { -1.0f };
    std::atomic<bool> cut { false }, writing { false };
    static_assert (std::atomic<float>::is_always_lock_free);

    // The audio thread's: its level's gain as the last block ended, and its cut's (1 sending, 0 cut); below 0 for none yet.
    // Whether the last block followed its automation or modulation, and how many samples are left of the ease after it.
    float level = -1.0f, heard = -1.0f;
    bool followedLast = false;
    int easing = 0;
};

} // namespace phazite::engine
